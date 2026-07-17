// ELF64 ET_REL relocatable-object MEMBER READER tests -- cycle c164,
// anchor D-LK-RELOCATABLE-OBJECT-READER (ELF slice).
//
// The reader (`src/link/format/elf_object_reader.cpp`) is the INVERSE of
// elf.cpp's ET_REL writer: it reconstructs a relocatable object's FULL
// linkable body back into an `AssembledModule` -- the exact structure the
// c154 cross-CU merge consumes -- unblocking the c165 static-link.
//
// Coverage:
//   1. DSS writer <-> reader FULL-object ROUND-TRIP (the self-contained
//      oracle): write a module with functions + rodata/data/relro data +
//      extern function + extern data + relocations, read it back, assert
//      every field class matches (names, byte ranges, relocation
//      {offset,target-by-name,kind,addend}, data sections, extern isData).
//      Red-on-disable is inherent per field class (drop the RELA parse ->
//      relocations empty -> assertion fails; drop the symtab -> functions
//      unnamed/unsliced -> assertion fails).
//   2. REAL gcc-produced .o cross-check (embedded golden fixture): read
//      genuine `gcc -c lib.c` bytes, assert the recovered functions,
//      extern, data objects, and relocations.
//   3. Bounds / truncation red-pins: header/section-table/symtab/rela past
//      EOF, wrong ELF class, wrong e_type, unknown reloc type -> fail loud
//      (nullopt + F_* diagnostic), never a crash or silent partial parse.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/object_format_schema.hpp"

#include "gcc_lib_c164_object.inc"
#include "gcc_section_relative_c167.inc"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dss;

namespace {

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShipped() {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(x86_64) failed";
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(elf64-x86_64-linux) failed";
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

// Resolve a reconstructed SymbolId to its name -- searches the module's
// defined ModuleSymbols first, then its extern imports. Empty if unknown.
[[nodiscard]] std::string nameOf(AssembledModule const& m, SymbolId id) {
    for (auto const& s : m.symbols) if (s.symbol == id) return s.name;
    for (auto const& e : m.externImports) if (e.symbol == id) return e.mangledName;
    return {};
}

[[nodiscard]] AssembledFunction const* funcNamed(AssembledModule const& m,
                                                 std::string const& name) {
    for (auto const& f : m.functions) {
        for (auto const& s : m.symbols) {
            if (s.symbol == f.symbol && s.name == name) return &f;
        }
    }
    return nullptr;
}

[[nodiscard]] AssembledData const* dataNamed(AssembledModule const& m,
                                             std::string const& name) {
    for (auto const& d : m.dataItems) {
        for (auto const& s : m.symbols) {
            if (s.symbol == d.symbol && s.name == name) return &d;
        }
    }
    return nullptr;
}

[[nodiscard]] ExternImport const* externNamed(AssembledModule const& m,
                                              std::string const& name) {
    for (auto const& e : m.externImports) if (e.mangledName == name) return &e;
    return nullptr;
}

// -- Little-endian byte pokers for the corruption red-pins ----------
[[nodiscard]] std::uint16_t rd16(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[o + 1]) << 8);
}
[[nodiscard]] std::uint64_t rd64(std::vector<std::uint8_t> const& b, std::size_t o) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[o + i]) << (i * 8);
    return v;
}
void wr64(std::vector<std::uint8_t>& b, std::size_t o, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[o + i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF);
}
// Section-header table file offset of the i-th section header.
[[nodiscard]] std::size_t shdrAt(std::vector<std::uint8_t> const& b, std::uint16_t i) {
    return static_cast<std::size_t>(rd64(b, 40)) + i * 64;
}
// First section header whose sh_type == type (0 = none).
[[nodiscard]] std::size_t firstShdrOfType(std::vector<std::uint8_t> const& b,
                                          std::uint32_t type) {
    std::uint16_t const shnum = rd16(b, 60);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::size_t const sh = shdrAt(b, i);
        if (static_cast<std::uint32_t>(rd64(b, sh + 4) & 0xFFFFFFFFu) == type) return sh;
    }
    return 0;
}
// .symtab file offset (SHT_SYMTAB = 2); symbol i's record at symtabBody + i*24.
[[nodiscard]] std::size_t symtabBody(std::vector<std::uint8_t> const& b) {
    std::size_t const sh = firstShdrOfType(b, 2);
    return sh ? static_cast<std::size_t>(rd64(b, sh + 24)) : 0;
}

// A defined function/data reloc target, resolved by name for the round-trip
// (raw SymbolId integers are per-CU and intentionally NOT preserved -- the
// merge matches by name).
[[nodiscard]] Relocation const*
relToName(AssembledModule const& m, AssembledFunction const& fn,
          std::string const& targetName) {
    for (auto const& r : fn.relocations) {
        if (nameOf(m, r.target) == targetName) return &r;
    }
    return nullptr;
}

} // namespace

// -- 1. DSS writer <-> reader full-object round-trip -----------------

TEST(RelocatableObjectReader, DssWriterRoundTripReconstructsEveryFieldClass) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    // A module exercising every reconstructable field class:
    //   * lib_add   -- a plain function (no relocations).
    //   * lib_greet -- a function with THREE relocations: a rel32 to a
    //                  DEFINED rodata object (msg), a rel32 to an extern
    //                  DATA object (env), and a rel32 CALL to an extern
    //                  FUNCTION (puts). The writer emits PC32 for the first
    //                  two and PLT32 for the call; the reader must map all
    //                  three back to the SAME rel32 RelocationKind and
    //                  infer isData from the PLT-vs-plain distinction.
    //   * msg       -- a Rodata data item.
    //   * counter   -- a Data data item.
    //   * vtable    -- a RelRoConst data item carrying an abs64 reloc to
    //                  lib_add (exercises .data.rel.ro + .rela.data.rel.ro).
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction add;
    add.symbol = SymbolId{1};
    add.bytes  = {0x8D, 0x04, 0x37, 0xC3};   // lea eax,[rdi+rsi]; ret
    mod.functions.push_back(add);

    AssembledFunction greet;
    greet.symbol = SymbolId{2};
    greet.bytes.assign(20, 0x90);            // 20 nops; only the relocs matter
    greet.relocations.push_back(Relocation{2u,  SymbolId{10}, RelocationKind{1}, 0});   // -> msg (defined data)
    greet.relocations.push_back(Relocation{8u,  SymbolId{20}, RelocationKind{1}, 0});   // -> env (extern data)
    greet.relocations.push_back(Relocation{14u, SymbolId{21}, RelocationKind{1}, 0});   // -> puts (extern fn, call)
    mod.functions.push_back(greet);

    AssembledData msg;
    msg.symbol    = SymbolId{10};
    msg.section   = DataSectionKind::Rodata;
    msg.bytes     = {'h', 'i', 0};
    msg.alignment = Alignment::of<1>();
    mod.dataItems.push_back(msg);

    AssembledData counter;
    counter.symbol    = SymbolId{11};
    counter.section   = DataSectionKind::Data;
    counter.bytes     = {7, 0, 0, 0};
    counter.alignment = Alignment::of<4>();
    mod.dataItems.push_back(counter);

    AssembledData vtable;
    vtable.symbol    = SymbolId{12};
    vtable.section   = DataSectionKind::RelRoConst;
    vtable.bytes     = {0, 0, 0, 0, 0, 0, 0, 0};
    vtable.alignment = Alignment::of<8>();
    vtable.relocations.push_back(Relocation{0u, SymbolId{1}, RelocationKind{2}, 0});    // abs64 -> lib_add
    mod.dataItems.push_back(vtable);

    // Real names for the defined symbols (Global -> real name in the .o).
    // All Global: DSS's writer carves a LOCAL symbol's NAME to `sym_<id>`
    // (the name carve-out) and emits it STB_GLOBAL, so a Local name does not
    // round-trip through DSS's OWN writer -- that carve-out is a writer
    // property, not a reader gap (Local preservation is pinned against the
    // real gcc fixture below, where `msg` is genuinely STB_LOCAL with a real
    // name). Here we use Global names so every identity round-trips.
    mod.symbols = {
        ModuleSymbol{SymbolId{1},  "lib_add",   SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{2},  "lib_greet", SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{10}, "msg",       SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{11}, "counter",   SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{12}, "vtable",    SymbolBinding::Global, SymbolVisibility::Default},
    };
    // Extern imports: a DATA extern (env) and a FUNCTION extern (puts).
    mod.externImports = {
        ExternImport{SymbolId{20}, "env",  "libc.so.6", /*isData=*/true},
        ExternImport{SymbolId{21}, "puts", "libc.so.6", /*isData=*/false},
    };

    DiagnosticReporter wrep;
    auto objBytes = elf::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u) << "writer must accept the module";
    ASSERT_FALSE(objBytes.empty());

    DiagnosticReporter rrep;
    auto readOpt = elf::readRelocatableObject(objBytes, *loaded.target,
                                              *loaded.format, rrep);
    ASSERT_TRUE(readOpt.has_value())
        << "reader must reconstruct the module (errors="
        << rrep.errorCount() << ")";
    ASSERT_EQ(rrep.errorCount(), 0u);
    AssembledModule const& got = *readOpt;

    // -- functions: names + byte ranges (symtab + .text slicing) --
    ASSERT_EQ(got.functions.size(), 2u);
    auto const* rAdd = funcNamed(got, "lib_add");
    auto const* rGreet = funcNamed(got, "lib_greet");
    ASSERT_NE(rAdd, nullptr) << "lib_add must be recovered by name (red-on-disable "
                               "vs a dropped symtab parse)";
    ASSERT_NE(rGreet, nullptr);
    EXPECT_EQ(rAdd->bytes, add.bytes) << ".text sliced by (st_value, st_size)";
    EXPECT_EQ(rGreet->bytes, greet.bytes);
    EXPECT_TRUE(rAdd->relocations.empty());

    // -- .rela.text relocations (offset relative to function start, kind
    //    mapped back from nativeId, addend un-baked, target by name) --
    ASSERT_EQ(rGreet->relocations.size(), 3u)
        << "all three .rela.text entries must land on lib_greet "
           "(red-on-disable vs a dropped RELA parse)";
    auto const* rMsg = relToName(got, *rGreet, "msg");
    auto const* rEnv = relToName(got, *rGreet, "env");
    auto const* rPuts = relToName(got, *rGreet, "puts");
    ASSERT_NE(rMsg, nullptr);
    ASSERT_NE(rEnv, nullptr);
    ASSERT_NE(rPuts, nullptr);
    EXPECT_EQ(rMsg->offset, 2u);
    EXPECT_EQ(rEnv->offset, 8u);
    EXPECT_EQ(rPuts->offset, 14u);
    // Every one maps back to the SAME rel32 kind (PC32 AND PLT32 -> kind 1).
    EXPECT_EQ(rMsg->kind, RelocationKind{1});
    EXPECT_EQ(rEnv->kind, RelocationKind{1});
    EXPECT_EQ(rPuts->kind, RelocationKind{1});
    // Addend un-baked: the writer stored r_addend = 0 + addendBias(-4) = -4;
    // the reader recovers the DSS-native 0 (red-on-disable vs keeping -4).
    EXPECT_EQ(rMsg->addend, 0);
    EXPECT_EQ(rEnv->addend, 0);
    EXPECT_EQ(rPuts->addend, 0);

    // -- data items: sections + bytes + names --
    auto const* dMsg = dataNamed(got, "msg");
    auto const* dCounter = dataNamed(got, "counter");
    auto const* dVtable = dataNamed(got, "vtable");
    ASSERT_NE(dMsg, nullptr);
    ASSERT_NE(dCounter, nullptr);
    ASSERT_NE(dVtable, nullptr);
    EXPECT_EQ(dMsg->section, DataSectionKind::Rodata);
    EXPECT_EQ(dMsg->bytes, msg.bytes);
    EXPECT_EQ(dCounter->section, DataSectionKind::Data);
    EXPECT_EQ(dCounter->bytes, counter.bytes);
    EXPECT_EQ(dVtable->section, DataSectionKind::RelRoConst);
    EXPECT_EQ(dVtable->bytes.size(), 8u);

    // -- data-item relocation (.rela.data.rel.ro): abs64 -> lib_add --
    ASSERT_EQ(dVtable->relocations.size(), 1u)
        << "the relro item's own relocation must be recovered from "
           ".rela.data.rel.ro";
    EXPECT_EQ(dVtable->relocations[0].offset, 0u);
    EXPECT_EQ(dVtable->relocations[0].kind, RelocationKind{2});   // abs64
    EXPECT_EQ(dVtable->relocations[0].addend, 0);
    EXPECT_EQ(nameOf(got, dVtable->relocations[0].target), "lib_add");

    // -- extern imports: names + isData INFERENCE (PLT -> fn, plain -> data) --
    auto const* ePuts = externNamed(got, "puts");
    auto const* eEnv = externNamed(got, "env");
    ASSERT_NE(ePuts, nullptr);
    ASSERT_NE(eEnv, nullptr);
    EXPECT_FALSE(ePuts->isData)
        << "puts is reached via a PLT32 reloc -> inferred a FUNCTION import";
    EXPECT_TRUE(eEnv->isData)
        << "env is reached via a plain PC32 reloc -> inferred a DATA import";

    // -- the module is well-formed for the merge --
    EXPECT_EQ(got.expectedFuncCount, 2u);
    EXPECT_TRUE(got.ok());
}

// STB_LOCAL vs STB_GLOBAL binding is recovered from a REAL object (gcc emits
// the `static const char msg[]` as STB_LOCAL and the `int lib_add()` as
// STB_GLOBAL). Red-on-disable vs the reader defaulting every symbol to one
// binding -- the merge relies on Local to keep a static module-private.
TEST(RelocatableObjectReader, BindingRecoveredFromRealGccObject) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> const obj = dss::test::gccLibC164Object();
    DiagnosticReporter rrep;
    auto got = elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value());

    auto bindingOf = [&](std::string const& name) -> std::optional<SymbolBinding> {
        for (auto const& s : got->symbols) if (s.name == name) return s.binding;
        return std::nullopt;
    };
    auto msgBind = bindingOf("msg");
    auto addBind = bindingOf("lib_add");
    ASSERT_TRUE(msgBind.has_value()) << "the static `msg` object must have a ModuleSymbol";
    ASSERT_TRUE(addBind.has_value());
    EXPECT_EQ(*msgBind, SymbolBinding::Local)
        << "gcc's `static const char msg[]` is STB_LOCAL -- must round-trip Local";
    EXPECT_EQ(*addBind, SymbolBinding::Global)
        << "gcc's `int lib_add()` is STB_GLOBAL";
}

// AGNOSTICISM: the SAME reader reconstructs an arm64 ELF ET_REL object,
// mapping the arm64-native reloc ids (R_AARCH64_ABS64 = 257) back to the
// universal RelocationKind via the arm64 FORMAT schema -- no hardcoded
// x86 reloc numbers in the reader. Red-on-disable vs a machine branch.
TEST(RelocatableObjectReader, Aarch64RoundTripIsMachineAgnostic) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(fmt.has_value());

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};   // ret
    mod.functions.push_back(fn);
    AssembledData vt;
    vt.symbol    = SymbolId{2};
    vt.section   = DataSectionKind::RelRoConst;
    vt.bytes     = {0, 0, 0, 0, 0, 0, 0, 0};
    vt.alignment = Alignment::of<8>();
    vt.relocations.push_back(Relocation{0u, SymbolId{1}, RelocationKind{4}, 0});  // arm64 abs64
    mod.dataItems.push_back(vt);
    mod.symbols = {
        ModuleSymbol{SymbolId{1}, "f",  SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{2}, "vt", SymbolBinding::Global, SymbolVisibility::Default},
    };

    DiagnosticReporter wrep;
    auto objBytes = elf::encode(mod, **target, **fmt, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);

    DiagnosticReporter rrep;
    auto got = elf::readRelocatableObject(objBytes, **target, **fmt, rrep);
    ASSERT_TRUE(got.has_value()) << "arm64 object must reconstruct (errors="
                                 << rrep.errorCount() << ")";
    ASSERT_EQ(got->dataItems.size(), 1u);
    ASSERT_EQ(got->dataItems[0].relocations.size(), 1u);
    EXPECT_EQ(got->dataItems[0].relocations[0].kind, RelocationKind{4})
        << "R_AARCH64_ABS64 (257) mapped back to the arm64 abs64 kind via the schema";
    EXPECT_EQ(nameOf(*got, got->dataItems[0].relocations[0].target), "f");
}

// -- 2. Real gcc-produced .o cross-check (embedded golden) -----------

TEST(RelocatableObjectReader, ReadsRealGccObjectFunctionsSymbolsRelocations) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> const obj = dss::test::gccLibC164Object();
    ASSERT_GT(obj.size(), 64u);

    DiagnosticReporter rrep;
    auto readOpt = elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(readOpt.has_value())
        << "reader must parse a real gcc .o (errors=" << rrep.errorCount() << ")";
    EXPECT_EQ(rrep.errorCount(), 0u);
    AssembledModule const& got = *readOpt;

    // Functions: lib_add (24 bytes @ .text 0) + lib_greet (25 bytes @ .text 0x18).
    auto const* add = funcNamed(got, "lib_add");
    auto const* greet = funcNamed(got, "lib_greet");
    ASSERT_NE(add, nullptr) << "lib_add recovered from a real gcc symtab";
    ASSERT_NE(greet, nullptr) << "lib_greet recovered from a real gcc symtab";
    EXPECT_EQ(add->bytes.size(), 24u);
    EXPECT_EQ(greet->bytes.size(), 25u);

    // Extern import: puts (SHN_UNDEF NOTYPE).
    auto const* puts = externNamed(got, "puts");
    ASSERT_NE(puts, nullptr) << "the undefined `puts` reference must become an extern import";

    // Data objects: lib_counter (.data, 4 bytes) + msg (.rodata, 12 bytes).
    auto const* counter = dataNamed(got, "lib_counter");
    ASSERT_NE(counter, nullptr);
    EXPECT_EQ(counter->section, DataSectionKind::Data);
    EXPECT_EQ(counter->bytes.size(), 4u);
    auto const* msg = dataNamed(got, "msg");
    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->section, DataSectionKind::Rodata);
    EXPECT_EQ(msg->bytes.size(), 12u);

    // lib_greet carries the `call puts` relocation (PLT32 -> rel32 kind) and
    // the section-relative `.rodata` reference to msg. Both recovered as
    // rel32-kind relocations on lib_greet.
    ASSERT_EQ(greet->relocations.size(), 2u)
        << "lib_greet's two .rela.text entries (msg ref + puts call)";
    bool sawPutsCall = false;
    bool sawMsgRef = false;
    for (auto const& r : greet->relocations) {
        EXPECT_EQ(r.kind, RelocationKind{1}) << "both are rel32-family";
        EXPECT_EQ(r.addend, 0) << "gcc's -4 addend is the rel32 bias, un-baked to 0";
        if (nameOf(got, r.target) == "puts") sawPutsCall = true;
        if (nameOf(got, r.target) == "msg") sawMsgRef = true;
    }
    EXPECT_TRUE(sawPutsCall) << "one reloc must target the extern `puts`";
    // c167 SECTION-RELATIVE RESOLUTION (red-on-disable): gcc references `msg`
    // via the `.rodata` SECTION symbol (`R_X86_64_PC32 .rodata-4`), NOT `msg`
    // directly. The reader must REDIRECT that section-relative reference to the
    // `msg` data atom (offset 0, residual 0). Pre-c167 this reloc targeted a
    // bodiless `.rodata` section symbol -> nameOf == ".rodata", sawMsgRef false.
    EXPECT_TRUE(sawMsgRef)
        << "the .rodata section-relative reference must resolve to the `msg` atom";
}

// -- c167: section-relative relocations (anonymous content -> gap atoms) ----
//
// gcc references string literals / jump tables through a SECTION symbol + addend
// (`R_X86_64_PC32 .rodata-4`); the bytes have NO symbol of their own. The reader
// must reconstruct the anonymous region as a synthetic gap atom AND redirect the
// section-relative reference to it. D-LK-STATIC-LINK-SECTION-RELATIVE-RELOC.

// A `const char*` returning a string literal: the 12-byte "hello world" lives in
// an anonymous `.rodata` referenced via the `.rodata` section symbol. The reader
// reconstructs a gap atom with the bytes and binds the reference to it (residual
// 0). Red-on-disable: without the redirect the reference targets a bodiless
// `.rodata` section symbol; without gap reconstruction the bytes are lost.
TEST(RelocatableObjectReader, SectionRelativeStringLiteralReconstructsGapAtom) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter rep;
    auto got = elf::readRelocatableObject(dss::test::gccStrlitObject(),
                                          *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errors=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    // The anonymous "hello world\0" (12 bytes) is reconstructed as a data atom.
    std::string const want = "hello world";
    AssembledData const* gap = nullptr;
    for (auto const& d : got->dataItems) {
        if (d.bytes.size() == 12u
            && std::equal(want.begin(), want.end(), d.bytes.begin())) {
            gap = &d;
        }
    }
    ASSERT_NE(gap, nullptr)
        << "the anonymous string must be reconstructed as a synthetic gap atom";
    EXPECT_EQ(gap->section, DataSectionKind::Rodata);
    EXPECT_TRUE(nameOf(*got, gap->symbol).empty())
        << "the gap atom is anonymous (module-private, no ModuleSymbol)";

    // dss_greet's ONE section-relative reference resolves to the gap atom.
    auto const* greet = funcNamed(*got, "dss_greet");
    ASSERT_NE(greet, nullptr);
    ASSERT_EQ(greet->relocations.size(), 1u);
    EXPECT_EQ(greet->relocations[0].target, gap->symbol)
        << "the `.rodata-4` section-relative reloc must redirect to the string gap atom";
    EXPECT_EQ(greet->relocations[0].addend, 0) << "the string is at offset 0 -> residual 0";
}

// A dense `switch` -> an anonymous `.rodata` jump table (gap atom) whose entries
// are PC-relative SELF-references into the containing function. Exercises: (a)
// gap-atom reconstruction of the table; (b) the `.rela.text` lea-of-table refs
// redirecting to the gap atom; (c) the `.rela.rodata` entries redirecting to the
// function at INTERIOR offsets -- the data-section-PC-relative search-offset path.
TEST(RelocatableObjectReader, SectionRelativeJumpTableResolvesInteriorAndGapAtom) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter rep;
    auto got = elf::readRelocatableObject(dss::test::gccAnswerJumpTableObject(),
                                          *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errors=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    auto const* fn = funcNamed(*got, "lib_answer");
    auto const* pad = funcNamed(*got, "lib_pad");
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(pad, nullptr)
        << "both functions of the shared .text must reconstruct (lib_pad + lib_answer)";
    // lib_pad occupies .text [0, 0x19); lib_answer FOLLOWS at 0x19 -- so the
    // jump table's interior references must SELECT lib_answer, not lib_pad.

    // The 24-byte anonymous jump table -> a gap atom carrying 6 PC-relative
    // entries, each an INTERIOR reference into lib_answer.
    AssembledData const* jt = nullptr;
    for (auto const& d : got->dataItems) {
        if (d.relocations.size() == 6u) jt = &d;
    }
    ASSERT_NE(jt, nullptr) << "the jump table must reconstruct as a 6-entry gap atom";
    EXPECT_EQ(jt->bytes.size(), 24u);
    for (auto const& r : jt->relocations) {
        EXPECT_EQ(r.target, fn->symbol)
            << "each jump-table entry must SELECT the containing function lib_answer";
        EXPECT_NE(r.target, pad->symbol)
            << "the interior offsets fall inside lib_answer (@0x19+), NOT lib_pad -- "
               "the atom-selection path (multi-function .text) is locked here";
    }

    // lib_answer's 2 `.rela.text` lea-of-table refs redirect to the gap atom.
    ASSERT_EQ(fn->relocations.size(), 2u);
    for (auto const& r : fn->relocations) {
        EXPECT_EQ(r.target, jt->symbol)
            << "the lea-of-table refs resolve to the jump-table gap atom";
        EXPECT_EQ(r.addend, 0) << "the table base is at .rodata offset 0 -> residual 0";
    }
    EXPECT_TRUE(pad->relocations.empty()) << "lib_pad has no relocations";
}

// -- 3. Bounds / truncation / corruption red-pins --------------------

namespace {
[[nodiscard]] std::vector<std::uint8_t> validObject(Loaded const& loaded) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0};   // call rel32 -> extern
    fn.relocations.push_back(Relocation{1u, SymbolId{2}, RelocationKind{1}, 0});
    mod.functions.push_back(fn);
    mod.symbols = {ModuleSymbol{SymbolId{1}, "f", SymbolBinding::Global, SymbolVisibility::Default}};
    mod.externImports = {ExternImport{SymbolId{2}, "g", "libc.so.6", false}};
    DiagnosticReporter rep;
    return elf::encode(mod, *loaded.target, *loaded.format, rep);
}
} // namespace

TEST(RelocatableObjectReader, TruncationAtEveryLengthFailsLoudNeverCrashes) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto full = validObject(loaded);
    ASSERT_GT(full.size(), 64u);

    // Sanity: the full object reads back cleanly.
    {
        DiagnosticReporter rep;
        EXPECT_TRUE(elf::readRelocatableObject(full, *loaded.target, *loaded.format, rep)
                        .has_value());
    }
    // Every proper prefix must fail loud (nullopt + a diagnostic) -- the
    // section header table + symtab/strtab/rela bodies sit near EOF, so any
    // truncation makes some bounds check fire. Never a crash, never a silent
    // partial parse.
    for (std::size_t len = 1; len < full.size(); ++len) {
        std::vector<std::uint8_t> const trunc(full.begin(), full.begin() + len);
        DiagnosticReporter rep;
        auto got = elf::readRelocatableObject(trunc, *loaded.target, *loaded.format, rep);
        ASSERT_FALSE(got.has_value())
            << "truncation to " << len << " bytes must fail loud";
        EXPECT_GT(rep.errorCount(), 0u) << "a diagnostic must accompany the failure";
    }
}

TEST(RelocatableObjectReader, WrongElfClassFailsLoud) {
    auto loaded = loadShipped();
    auto obj = validObject(loaded);
    obj[4] = 1;   // EI_CLASS = ELFCLASS32
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_UnsupportedElfClass) saw = true;
    EXPECT_TRUE(saw) << "ELFCLASS32 must emit F_UnsupportedElfClass";
}

TEST(RelocatableObjectReader, NonRelObjectTypeFailsLoud) {
    auto loaded = loadShipped();
    auto obj = validObject(loaded);
    obj[16] = 2;   // e_type = ET_EXEC
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_UnsupportedBinaryFormat) saw = true;
    EXPECT_TRUE(saw) << "a non-ET_REL object must emit F_UnsupportedBinaryFormat";
}

TEST(RelocatableObjectReader, SectionTablePastEofFailsLoud) {
    auto loaded = loadShipped();
    auto obj = validObject(loaded);
    // e_shoff at bytes[40..47]: point it far past EOF.
    for (int i = 0; i < 8; ++i) obj[40 + i] = 0xFF;
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_CorruptedBinary) saw = true;
    EXPECT_TRUE(saw) << "a section table past EOF must emit F_CorruptedBinary";
}

TEST(RelocatableObjectReader, UnknownRelocTypeFailsLoud) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validObject(loaded);

    // Locate .rela.text by scanning section headers for SHT_RELA (type 4),
    // then corrupt the first entry's r_type (low 32 bits of r_info at
    // relaOff + 8) to a value the format schema does not declare.
    auto rdU64 = [&](std::size_t o) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(obj[o + i]) << (i * 8);
        return v;
    };
    auto rdU16 = [&](std::size_t o) {
        return static_cast<std::uint16_t>(obj[o]) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(obj[o + 1]) << 8);
    };
    std::uint64_t const shoff = rdU64(40);
    std::uint16_t const shnum = rdU16(60);
    std::size_t relaOff = 0;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::size_t const sh = static_cast<std::size_t>(shoff) + i * 64;
        std::uint32_t const type = static_cast<std::uint32_t>(rdU64(sh + 4) & 0xFFFFFFFFu);
        if (type == 4u) { relaOff = static_cast<std::size_t>(rdU64(sh + 24)); break; }
    }
    ASSERT_GT(relaOff, 0u) << ".rela.text must exist in the fixture object";
    // r_info low 32 bits at relaOff+8: set an undeclared reloc type (0x7777).
    obj[relaOff + 8] = 0x77;
    obj[relaOff + 9] = 0x77;

    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value())
        << "an undeclared reloc type must not silently drop -- fail loud";
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_CorruptedBinary) saw = true;
    EXPECT_TRUE(saw);
}

TEST(RelocatableObjectReader, NobitsTextSectionFailsLoudNeverReadsPastEof) {
    // A NOBITS section carries no file bytes and its sh_offset/sh_size are not
    // file-bounds-checked (a zero-fill section legitimately points past the
    // file data). If `.text` is (corruptly) SHT_NOBITS, slicing a function
    // body out of it would read past EOF -- the reader must fail loud, never
    // read out of file. Flip `.text`'s sh_type to NOBITS(8) and assert.
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validObject(loaded);
    auto rdU64 = [&](std::size_t o) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(obj[o + i]) << (i * 8);
        return v;
    };
    auto rdU16 = [&](std::size_t o) {
        return static_cast<std::uint16_t>(obj[o]) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(obj[o + 1]) << 8);
    };
    std::uint64_t const shoff = rdU64(40);
    std::uint16_t const shnum = rdU16(60);
    bool flipped = false;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::size_t const sh = static_cast<std::size_t>(shoff) + i * 64;
        std::uint64_t const flags = rdU64(sh + 8);
        if (flags & 0x4u) {   // SHF_EXECINSTR -> .text
            obj[sh + 4] = 8;  // sh_type = SHT_NOBITS
            obj[sh + 5] = obj[sh + 6] = obj[sh + 7] = 0;
            flipped = true;
            break;
        }
    }
    ASSERT_TRUE(flipped) << ".text (SHF_EXECINSTR) must exist in the fixture";
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value())
        << "a NOBITS .text must fail loud, never yield an out-of-file read";
    EXPECT_GT(rep.errorCount(), 0u);
}

// HIGH-2 (x86_64): an address-taken extern FUNCTION -- one both CALLED (a
// rel32 -> PLT32) and address-referenced (an abs64 `&cb` in a data item) --
// must stay isData==false. RED-on-disable: the old "any non-PLT reloc => data"
// rule saw the abs64 and flipped it to data (a spurious data import at c165).
TEST(RelocatableObjectReader, AddressTakenExternFunctionStaysFunction) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    caller.bytes  = {0xE8, 0, 0, 0, 0};   // call rel32 -> cb
    caller.relocations.push_back(Relocation{1u, SymbolId{9}, RelocationKind{1}, 0});  // PLT32 call
    mod.functions.push_back(caller);
    AssembledData table;                  // a const fn-ptr slot holding &cb
    table.symbol    = SymbolId{2};
    table.section   = DataSectionKind::RelRoConst;
    table.bytes     = {0, 0, 0, 0, 0, 0, 0, 0};
    table.alignment = Alignment::of<8>();
    table.relocations.push_back(Relocation{0u, SymbolId{9}, RelocationKind{2}, 0});   // abs64 &cb
    mod.dataItems.push_back(table);
    mod.symbols = {
        ModuleSymbol{SymbolId{1}, "caller", SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{2}, "cbtab",  SymbolBinding::Global, SymbolVisibility::Default},
    };
    mod.externImports = {ExternImport{SymbolId{9}, "cb", "libc.so.6", /*isData=*/false}};

    DiagnosticReporter wrep;
    auto objBytes = elf::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    DiagnosticReporter rrep;
    auto got = elf::readRelocatableObject(objBytes, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value());
    auto const* cb = externNamed(*got, "cb");
    ASSERT_NE(cb, nullptr);
    EXPECT_FALSE(cb->isData)
        << "an extern reached by a PLT32 call is a FUNCTION even when also "
           "address-taken via abs64";
}

// HIGH-3: a -ffunction-sections/-fdata-sections gcc object houses each body in
// its OWN section (`.text.lib_add`, `.rodata.msg`, `.data.lib_counter`) --
// names the schema does not declare. The reader must RECOVER them by prefix/
// flags, never silently drop. RED-on-disable: exact-name-only matching leaves
// each a bodiless ModuleSymbol (or now fails loud), so funcNamed/dataNamed miss.
TEST(RelocatableObjectReader, FunctionSectionsBodiesRecoveredNotDropped) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<std::uint8_t> const obj = dss::test::gccLibC164ObjectFunctionSections();
    DiagnosticReporter rrep;
    auto got = elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value())
        << "a -ffunction-sections object must reconstruct (errors="
        << rrep.errorCount() << ")";
    auto const* add = funcNamed(*got, "lib_add");
    auto const* greet = funcNamed(*got, "lib_greet");
    ASSERT_NE(add, nullptr) << "lib_add in `.text.lib_add` must be recovered by prefix";
    ASSERT_NE(greet, nullptr) << "lib_greet in `.text.lib_greet` must be recovered";
    EXPECT_EQ(add->bytes.size(), 24u);
    EXPECT_EQ(greet->bytes.size(), 25u);
    auto const* counter = dataNamed(*got, "lib_counter");
    ASSERT_NE(counter, nullptr) << "lib_counter in `.data.lib_counter` recovered";
    EXPECT_EQ(counter->section, DataSectionKind::Data);
    ASSERT_NE(dataNamed(*got, "msg"), nullptr) << "msg in `.rodata.msg` recovered";
    // lib_greet still carries the `call puts` relocation.
    bool sawPuts = false;
    for (auto const& r : greet->relocations)
        if (nameOf(*got, r.target) == "puts") sawPuts = true;
    EXPECT_TRUE(sawPuts);
}

// HIGH-2 (aarch64): a REAL aarch64 gcc object reaches the extern `puts` via
// R_AARCH64_CALL26 -- a call reloc with NO PLT-variant id in the schema. The
// reader must still type puts a FUNCTION (via the CALL26 branch formula), not
// misclassify it as data. RED-on-disable: the old PLT-only rule (pltNativeIds
// empty on aarch64) marked EVERY aarch64 extern call as data.
TEST(RelocatableObjectReader, Aarch64ExternCallTypedAsFunctionNotData) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(fmt.has_value());
    std::vector<std::uint8_t> const obj = dss::test::gccLibC164ObjectAarch64();
    DiagnosticReporter rrep;
    auto got = elf::readRelocatableObject(obj, **target, **fmt, rrep);
    ASSERT_TRUE(got.has_value())
        << "a real aarch64 gcc .o must reconstruct (errors=" << rrep.errorCount() << ")";
    ASSERT_NE(funcNamed(*got, "lib_add"), nullptr);
    auto const* puts = externNamed(*got, "puts");
    ASSERT_NE(puts, nullptr) << "the UND `puts` reference must become an extern import";
    EXPECT_FALSE(puts->isData)
        << "puts is reached by R_AARCH64_CALL26 (a call) -- must be a FUNCTION import";
}

// CRITICAL red-pin: a crafted `.o` whose e_shstrndx section is SHT_NOBITS with
// sh_offset/sh_size past EOF must fail loud, never let `rdName` walk past the
// buffer. (The header loop skips the file-bounds check for NOBITS; the new
// SHT_STRTAB gate on the shstrtab is the belt.)
TEST(RelocatableObjectReader, ShstrtabNobitsPastEofFailsLoudNoOob) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validObject(loaded);
    std::uint16_t const shstrndx = rd16(obj, 62);
    std::size_t const sh = shdrAt(obj, shstrndx);
    // sh_type = SHT_NOBITS(8); sh_offset + sh_size point far past EOF.
    obj[sh + 4] = 8; obj[sh + 5] = obj[sh + 6] = obj[sh + 7] = 0;
    wr64(obj, sh + 24, 0xFFFFFFF0ull);   // sh_offset
    wr64(obj, sh + 32, 0x100000ull);     // sh_size
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value())
        << "a NOBITS/out-of-file .shstrtab must fail loud, never read past EOF";
    EXPECT_GT(rep.errorCount(), 0u);
}

// A defined symbol naming a section index past the section table (but below the
// reserved range) is corrupt -- fail loud, not a silent drop.
TEST(RelocatableObjectReader, OutOfRangeStShndxFailsLoud) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validObject(loaded);
    std::size_t const st = symtabBody(obj);
    ASSERT_GT(st, 0u);
    std::uint16_t const shnum = rd16(obj, 60);
    // Symbol index 2 is the defined function `f` (0=UNDEF, 1=.text SECTION,
    // 2=f). Point its st_shndx (sym+6) past the section table (still < 0xff00).
    std::size_t const shndxOff = st + 2 * 24 + 6;
    std::uint16_t const bad = static_cast<std::uint16_t>(shnum + 5);
    obj[shndxOff] = static_cast<std::uint8_t>(bad & 0xFF);
    obj[shndxOff + 1] = static_cast<std::uint8_t>((bad >> 8) & 0xFF);
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    EXPECT_GT(rep.errorCount(), 0u);
}

// A function symbol whose (st_value + st_size) exceeds its `.text` section
// must fail loud (the slice bounds belt), never over-read.
TEST(RelocatableObjectReader, FunctionRangePastTextFailsLoud) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validObject(loaded);
    std::size_t const st = symtabBody(obj);
    ASSERT_GT(st, 0u);
    // Symbol 2 = `f`. Blow up its st_size (sym+16) far past `.text`.
    wr64(obj, st + 2 * 24 + 16, 0x1000000ull);
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    EXPECT_GT(rep.errorCount(), 0u);
}

// A relocation naming a symbol index past the symbol table must fail loud.
TEST(RelocatableObjectReader, RelocSymbolIndexPastCountFailsLoud) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validObject(loaded);
    std::size_t const relaSh = firstShdrOfType(obj, 4);   // SHT_RELA
    ASSERT_GT(relaSh, 0u);
    std::size_t const relaBody = static_cast<std::size_t>(rd64(obj, relaSh + 24));
    // r_info at relaBody+8: high 32 bits = symbol index. Set it huge.
    obj[relaBody + 12] = 0xFF; obj[relaBody + 13] = 0xFF;
    obj[relaBody + 14] = 0xFF; obj[relaBody + 15] = 0x7F;
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    EXPECT_GT(rep.errorCount(), 0u);
}

// Two STT_FUNC symbols with DIFFERENT-start overlapping ranges must fail loud
// (a relocation in the overlap would mis-route via findInterval).
TEST(RelocatableObjectReader, OverlappingFunctionRangesFailsLoud) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    // Two functions, no externs: symtab 0=UNDEF, 1=.text SECTION, 2=fn1, 3=fn2.
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction f1; f1.symbol = SymbolId{1}; f1.bytes.assign(8, 0x90);
    AssembledFunction f2; f2.symbol = SymbolId{2}; f2.bytes.assign(8, 0x90);
    mod.functions = {f1, f2};
    mod.symbols = {
        ModuleSymbol{SymbolId{1}, "fn1", SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{2}, "fn2", SymbolBinding::Global, SymbolVisibility::Default},
    };
    DiagnosticReporter wrep;
    auto obj = elf::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    // fn2 is at .text offset 8 (after fn1's 8 bytes). Move its st_value to 4 so
    // [4,12) partially overlaps fn1's [0,8) with a DIFFERENT start.
    std::size_t const st = symtabBody(obj);
    ASSERT_GT(st, 0u);
    wr64(obj, st + 3 * 24 + 8, 4ull);   // symbol 3 = fn2, st_value -> 4
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value())
        << "different-start overlapping function ranges must fail loud";
    EXPECT_GT(rep.errorCount(), 0u);
}

// LOW guard: a format schema whose reverse map is AMBIGUOUS -- two reloc rows
// sharing a nativeId but mapping to different kinds -- must fail loud rather
// than let "last row wins" silently mis-decode. (The schema validator does not
// enforce nativeId uniqueness, so this is the reader's belt.)
TEST(RelocatableObjectReader, DuplicateNativeIdInSchemaFailsLoud) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validObject(loaded);   // a structurally valid ELF object
    auto collide = ObjectFormatSchema::loadFromText(
        R"({"dssObjectFormatVersion":1,"dataModel":"LP64",
            "format":{"name":"elf-collide","kind":"elf"},
            "elf":{"class":"elf64","data":"lsb","osabi":"sysv","abiVersion":0,"machine":62},
            "relocations":[{"name":"A","kind":1,"nativeId":7},
                           {"name":"B","kind":2,"nativeId":7}]})");
    ASSERT_TRUE(collide.has_value())
        << "the colliding schema must load (the validator does not enforce "
           "nativeId uniqueness -- the reader is the belt)";
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, **collide, rep).has_value())
        << "an ambiguous nativeId reverse map must fail loud";
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(RelocatableObjectReader, NonElfFormatSchemaFailsLoud) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validObject(loaded);
    // A wasm format schema cannot parse an ELF object.
    auto wasm = ObjectFormatSchema::loadFromText(
        R"({"dssObjectFormatVersion":1,"dataModel":"LP64",
            "format":{"name":"w","kind":"wasm"}})");
    ASSERT_TRUE(wasm.has_value());
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(obj, *loaded.target, **wasm, rep).has_value());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_UnsupportedBinaryFormat) saw = true;
    EXPECT_TRUE(saw);
}
