// Windows COFF `.obj` relocatable-object MEMBER READER tests -- cycle c170,
// anchor D-LK-RELOCATABLE-OBJECT-READER-COFF.
//
// The reader (`src/link/format/coff_object_reader.cpp`) is the INVERSE of
// pe.cpp's Obj-arm writer: it reconstructs a relocatable object's FULL
// linkable body back into an `AssembledModule` -- the exact structure the
// c154 cross-CU merge consumes -- the PE/COFF sibling of the c164 ELF +
// c168 Mach-O readers, unblocking the c165 static-link for Windows `.lib`
// members.
//
// Coverage:
//   1. DSS writer <-> reader FULL-object ROUND-TRIP (the self-contained
//      oracle): write a module with 2 functions + rodata/data/relro data +
//      extern function + extern data + relocations (incl. a data ADDR64 with
//      a NON-zero in-slot addend), read it back, assert every field class
//      matches (function names + byte ranges sliced by sorted Value, data
//      sections + bytes, relocation {offset, target-by-name, kind, addend},
//      extern names + isData). Red-on-disable is inherent per field class.
//   2. Multi-item-per-section slicing (VALUE correctness): two data items in
//      one section -> two atoms, real content at offset 0, a reloc routes
//      correctly across the slice.
//   3. Truncation-at-every-length fuzz -> every proper prefix fails loud
//      (nullopt + diagnostic), never crashes.
//   4. Corruption red-pins: nonzero SizeOfOptionalHeader (a PE image);
//      unknown reloc nativeId; a reloc SymbolTableIndex past NumberOfSymbols;
//      a SectionNumber past NumberOfSections -> all fail loud.
//   5. Non-PE format schema -> fail loud.
//   6. Extern IMAGE_SYMBOL DTYPE_FUNCTION type hint -> isData=false inference
//      (the COFF-vs-Mach-O difference; red-on-disable vs a hardcoded seed).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/pe.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dss;

namespace {

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShipped(std::string_view targetName,
                                 std::string_view formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped(targetName);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped target " << targetName << " failed";
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped format " << formatName << " failed";
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

// A defined-or-extern reloc target, resolved by name (raw SymbolId integers
// are per-CU and intentionally NOT preserved -- the merge matches by name).
[[nodiscard]] Relocation const*
relToName(AssembledModule const& m, AssembledFunction const& fn,
          std::string const& targetName) {
    for (auto const& r : fn.relocations) {
        if (nameOf(m, r.target) == targetName) return &r;
    }
    return nullptr;
}

// -- Little-endian byte pokers for the corruption red-pins ----------
[[nodiscard]] std::uint16_t rd16(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o]) | (static_cast<std::uint16_t>(b[o + 1]) << 8);
}
[[nodiscard]] std::uint32_t rd32(std::vector<std::uint8_t> const& b, std::size_t o) {
    return  static_cast<std::uint32_t>(b[o])
         | (static_cast<std::uint32_t>(b[o + 1]) <<  8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16)
         | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
void wr16(std::vector<std::uint8_t>& b, std::size_t o, std::uint16_t v) {
    b[o]     = static_cast<std::uint8_t>(v & 0xFFu);
    b[o + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}
void wr32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu);
}

// IMAGE_FILE_HEADER field offsets (fixed) + section-header-0 field offsets.
constexpr std::size_t kFhSymTabPtr   = 8;    // u32 PointerToSymbolTable
constexpr std::size_t kFhNumSymbols  = 12;   // u32 NumberOfSymbols
constexpr std::size_t kFhOptHdrSize  = 16;   // u16 SizeOfOptionalHeader
constexpr std::size_t kSect0PtrReloc = 20 + 24;  // section 0 PointerToRelocations
constexpr std::size_t kSymbolRecordSz = 18;

// A minimal valid COFF `.obj`: one `.text` function whose `call rel32` is
// patched by a REL32 relocation against an undefined extern `g`. Two symbols
// (defined `f` at record 0, undefined `g` at record 1).
[[nodiscard]] std::vector<std::uint8_t> validObject(Loaded const& loaded) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction f;
    f.symbol = SymbolId{1};
    f.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00};   // call rel32 (patched by the reloc)
    f.relocations.push_back(Relocation{1u, SymbolId{2}, RelocationKind{1}, 0}); // REL32 -> g
    mod.functions.push_back(std::move(f));
    mod.symbols = {ModuleSymbol{SymbolId{1}, "f", SymbolBinding::Global,
                                SymbolVisibility::Default}};
    mod.externImports = {ExternImport{SymbolId{2}, "g", "msvcrt.dll", false}};
    DiagnosticReporter rep;
    return pe::encode(mod, *loaded.target, *loaded.format, rep);
}

// File offset of the extern IMAGE_SYMBOL record whose inline name is `name`
// (SectionNumber == 0). 0 if not found (0 is never a symbol-record offset).
[[nodiscard]] std::size_t
findExternSymbolRecord(std::vector<std::uint8_t> const& b, std::string const& name) {
    std::uint32_t const symtabPtr = rd32(b, kFhSymTabPtr);
    std::uint32_t const numSyms   = rd32(b, kFhNumSymbols);
    for (std::uint32_t i = 0; i < numSyms; ++i) {
        std::size_t const so =
            static_cast<std::size_t>(symtabPtr) + static_cast<std::size_t>(i) * kSymbolRecordSz;
        if (so + kSymbolRecordSz > b.size()) break;
        if (rd32(b, so) == 0u) continue;   // offset-form name -- ours are inline
        std::string inl;
        for (std::size_t n = 0; n < 8u && b[so + n] != 0u; ++n) {
            inl.push_back(static_cast<char>(b[so + n]));
        }
        if (inl == name && rd16(b, so + 12) == 0u) return so;   // SectionNumber @ +12
    }
    return 0;
}

} // namespace

// -- 1. DSS writer <-> reader full-object round-trip -----------------

TEST(CoffObjectReader, DssWriterRoundTripReconstructsEveryFieldClass) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    // A module exercising every reconstructable field class:
    //   * add     -- a leaf function (no relocations).
    //   * greet   -- a function with THREE `.text` relocations: a REL32 CALL
    //                to an extern FUNCTION (puts), an ADDR64 ref to a DEFINED
    //                rodata object (msg), and a REL32 ref to an extern DATA
    //                object (env).
    //   * msg     -- a Rodata data item (.rdata).
    //   * counter -- a Data data item (.data).
    //   * vtable  -- a RelRoConst data item (a SECOND .rdata) carrying an
    //                ADDR64 reloc to `add` with a NON-zero in-slot addend
    //                (exercises the COFF in-place data-slot addend read + the
    //                reloc-presence rodata-vs-relro disambiguation).
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction add;
    add.symbol = SymbolId{1};
    add.bytes  = {0xC3};                      // x86_64 RET
    mod.functions.push_back(add);

    AssembledFunction greet;
    greet.symbol = SymbolId{2};
    greet.bytes.assign(12, 0x90);             // 12 NOP filler; only the relocs matter
    greet.relocations.push_back(Relocation{0u, SymbolId{21}, RelocationKind{1}, 0}); // REL32  -> puts
    greet.relocations.push_back(Relocation{4u, SymbolId{10}, RelocationKind{2}, 0}); // ADDR64 -> msg
    greet.relocations.push_back(Relocation{8u, SymbolId{20}, RelocationKind{1}, 0}); // REL32  -> env
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
    vtable.relocations.push_back(Relocation{0u, SymbolId{1}, RelocationKind{2}, 8}); // ADDR64 -> add, addend 8
    mod.dataItems.push_back(vtable);

    // All Global names round-trip verbatim through DSS's OWN writer (it emits
    // every externally-visible defined symbol EXTERNAL with its real name;
    // COFF x64 C mangling is IDENTITY, so no leading underscore). We use
    // Global names so every identity round-trips (mirrors the ELF/Mach-O
    // reader round-trip discipline).
    mod.symbols = {
        ModuleSymbol{SymbolId{1},  "add",     SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{2},  "greet",   SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{10}, "msg",     SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{11}, "counter", SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{12}, "vtable",  SymbolBinding::Global, SymbolVisibility::Default},
    };
    mod.externImports = {
        ExternImport{SymbolId{20}, "env",  "kernel32.dll", /*isData=*/true},
        ExternImport{SymbolId{21}, "puts", "msvcrt.dll",   /*isData=*/false},
    };

    DiagnosticReporter wrep;
    auto objBytes = pe::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u) << "writer must accept the module";
    ASSERT_FALSE(objBytes.empty());

    DiagnosticReporter rrep;
    auto readOpt = pe::readRelocatableObject(objBytes, *loaded.target,
                                             *loaded.format, rrep);
    ASSERT_TRUE(readOpt.has_value())
        << "reader must reconstruct the module (errors=" << rrep.errorCount() << ")";
    ASSERT_EQ(rrep.errorCount(), 0u);
    AssembledModule const& got = *readOpt;

    // -- functions: names + byte ranges (IMAGE_SYMBOL Value slicing) --
    ASSERT_EQ(got.functions.size(), 2u);
    auto const* rAdd = funcNamed(got, "add");
    auto const* rGreet = funcNamed(got, "greet");
    ASSERT_NE(rAdd, nullptr) << "add must be recovered by name (red-on-disable "
                               "vs a dropped symbol parse)";
    ASSERT_NE(rGreet, nullptr);
    EXPECT_EQ(rAdd->bytes, add.bytes) << "`.text` sliced by sorted Value";
    EXPECT_EQ(rGreet->bytes, greet.bytes);
    EXPECT_TRUE(rAdd->relocations.empty());

    // -- `.text` relocations (offset relative to function start, kind mapped
    //    back from nativeId, addend 0, target by name) --
    ASSERT_EQ(rGreet->relocations.size(), 3u)
        << "all three `.text` relocs must land on greet "
           "(red-on-disable vs a dropped reloc-table parse)";
    auto const* rPuts = relToName(got, *rGreet, "puts");
    auto const* rMsg = relToName(got, *rGreet, "msg");
    auto const* rEnv = relToName(got, *rGreet, "env");
    ASSERT_NE(rPuts, nullptr);
    ASSERT_NE(rMsg, nullptr);
    ASSERT_NE(rEnv, nullptr);
    EXPECT_EQ(rPuts->offset, 0u);
    EXPECT_EQ(rMsg->offset, 4u);
    EXPECT_EQ(rEnv->offset, 8u);
    EXPECT_EQ(rPuts->kind, RelocationKind{1});   // REL32
    EXPECT_EQ(rMsg->kind, RelocationKind{2});    // ADDR64
    EXPECT_EQ(rEnv->kind, RelocationKind{1});    // REL32
    EXPECT_EQ(rPuts->addend, 0);                 // a `.text` reloc carries no addend
    EXPECT_EQ(rMsg->addend, 0);
    EXPECT_EQ(rEnv->addend, 0);

    // -- data items: sections + bytes + names --
    auto const* dMsg = dataNamed(got, "msg");
    auto const* dCounter = dataNamed(got, "counter");
    auto const* dVtable = dataNamed(got, "vtable");
    ASSERT_NE(dMsg, nullptr);
    ASSERT_NE(dCounter, nullptr);
    ASSERT_NE(dVtable, nullptr);
    EXPECT_EQ(dMsg->section, DataSectionKind::Rodata)
        << "msg resolves to the reloc-free `.rdata` (rodata) row";
    EXPECT_EQ(dMsg->bytes, msg.bytes);
    EXPECT_EQ(dCounter->section, DataSectionKind::Data);
    EXPECT_EQ(dCounter->bytes, counter.bytes);
    EXPECT_EQ(dVtable->section, DataSectionKind::RelRoConst)
        << "vtable resolves to the SECOND `.rdata` -- the SAME name as msg, "
           "distinguished ONLY by carrying its own IMAGE_RELOCATION table "
           "(the COFF reloc-presence disambiguator, no segment to key on)";
    EXPECT_EQ(dVtable->bytes.size(), 8u);

    // -- data-item relocation: ADDR64 -> add, addend READ FROM THE SLOT --
    ASSERT_EQ(dVtable->relocations.size(), 1u)
        << "the relro item's own relocation must be recovered from its "
           "section reloc table";
    EXPECT_EQ(dVtable->relocations[0].offset, 0u);
    EXPECT_EQ(dVtable->relocations[0].kind, RelocationKind{2});   // ADDR64
    EXPECT_EQ(dVtable->relocations[0].addend, 8)
        << "COFF has no addend column -- the addend must be recovered from the "
           "in-place slot bytes (red-on-disable vs a hardcoded 0)";
    EXPECT_EQ(dVtable->bytes[0], 8u)
        << "the writer baked the addend into the 8-byte slot; the reader "
           "reconstructs those literal bytes";
    EXPECT_EQ(nameOf(got, dVtable->relocations[0].target), "add");

    // -- extern imports: names + isData (LOSSLESS round-trip) --
    //
    // The c170 writer fold emits IMAGE_SYM_DTYPE_FUNCTION on a FUNCTION extern
    // (isData==false), so the function/data class round-trips FAITHFULLY: `puts`
    // (a called function) reconstructs isData=false, `env` (a data reference)
    // reconstructs isData=true. Red-on-disable: without the writer hint (or the
    // reader's `(Type & 0x30) == 0x20` mask) both would collapse to isData=true.
    auto const* ePuts = externNamed(got, "puts");
    auto const* eEnv = externNamed(got, "env");
    ASSERT_NE(ePuts, nullptr);
    ASSERT_NE(eEnv, nullptr);
    EXPECT_FALSE(ePuts->isData)
        << "puts is a FUNCTION extern -> the DTYPE_FUNCTION hint round-trips isData=false";
    EXPECT_TRUE(eEnv->isData)
        << "env is a DATA extern -> Type=0 round-trips isData=true";

    // -- the module is well-formed for the merge --
    EXPECT_EQ(got.expectedFuncCount, 2u);
}

// -- 1b. Multi-item-per-section slicing (VALUE correctness) ----------
//
// TWO named data items in ONE `.data` section of differing alignment: the
// writer packs them with alignment PADDING and records each item's PADDED
// SECTION-RELATIVE offset as its IMAGE_SYMBOL Value. Since IMAGE_SYMBOL
// carries no size, the reader slices the earlier item [off_0, off_1) and
// ABSORBS the trailing inter-item padding into it (value-benign). This test
// PINS that the absorption is not a silent corruption: two atoms reconstruct,
// each item's REAL content survives at offset 0 of its atom, and a reloc
// inside an item still routes to that item at the correct offset. It also
// locks the multi-item slice path the one-item-per-section round-trip never
// exercises.
TEST(CoffObjectReader, MultiItemSectionSlicesEachAtomValueCorrect) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction f;
    f.symbol = SymbolId{1};
    f.bytes  = {0xC3};   // x86_64 RET (the ADDR64 reloc target)
    mod.functions.push_back(f);

    // d0 (4 bytes, align 4) then d1 (8 bytes, align 8) -- BOTH `.data`, so d1
    // lands at a padded offset after d0 and the reader slices d0 to absorb the
    // gap. d1 carries an ADDR64 reloc to f (routes across the slice).
    AssembledData d0;
    d0.symbol    = SymbolId{10};
    d0.section   = DataSectionKind::Data;
    d0.bytes     = {0x11, 0x22, 0x33, 0x44};
    d0.alignment = Alignment::of<4>();
    mod.dataItems.push_back(d0);

    AssembledData d1;
    d1.symbol    = SymbolId{11};
    d1.section   = DataSectionKind::Data;
    d1.bytes     = {0, 0, 0, 0, 0, 0, 0, 0};
    d1.alignment = Alignment::of<8>();
    d1.relocations.push_back(Relocation{0u, SymbolId{1}, RelocationKind{2}, 0}); // ADDR64 -> f
    mod.dataItems.push_back(d1);

    mod.symbols = {
        ModuleSymbol{SymbolId{1},  "f",  SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{10}, "d0", SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{11}, "d1", SymbolBinding::Global, SymbolVisibility::Default},
    };

    DiagnosticReporter wrep;
    auto objBytes = pe::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_FALSE(objBytes.empty());

    DiagnosticReporter rrep;
    auto got = pe::readRelocatableObject(objBytes, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value()) << "errors=" << rrep.errorCount();
    ASSERT_EQ(rrep.errorCount(), 0u);

    // TWO separate atoms (the slice did not merge them into one).
    auto const* rd0 = dataNamed(*got, "d0");
    auto const* rd1 = dataNamed(*got, "d1");
    ASSERT_NE(rd0, nullptr);
    ASSERT_NE(rd1, nullptr);
    EXPECT_NE(rd0->symbol, rd1->symbol);

    // d0's REAL content survives at offset 0 (its atom may be padding-inflated
    // -- byte-exact size is the named follow-up -- but the value is intact).
    ASSERT_GE(rd0->bytes.size(), 4u);
    EXPECT_EQ(rd0->bytes[0], 0x11u);
    EXPECT_EQ(rd0->bytes[1], 0x22u);
    EXPECT_EQ(rd0->bytes[2], 0x33u);
    EXPECT_EQ(rd0->bytes[3], 0x44u);

    // d1's ADDR64 reloc routes to f at offset 0 -- reloc routing survives the
    // multi-item slice (red-on-disable vs a mis-attributed reloc).
    ASSERT_EQ(rd1->relocations.size(), 1u);
    EXPECT_EQ(rd1->relocations[0].offset, 0u);
    EXPECT_EQ(nameOf(*got, rd1->relocations[0].target), "f");
}

// -- 2. Truncation fuzz ----------------------------------------------

TEST(CoffObjectReader, TruncationAtEveryLengthFailsLoudNeverCrashes) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto full = validObject(loaded);
    ASSERT_GT(full.size(), 20u);

    // Sanity: the full object reads back cleanly.
    {
        DiagnosticReporter rep;
        EXPECT_TRUE(pe::readRelocatableObject(full, *loaded.target, *loaded.format, rep)
                        .has_value());
    }
    // Every proper prefix must fail loud (nullopt + a diagnostic) -- the
    // section header + `.text` body + reloc table + symtab + string table sit
    // past the file header, so any truncation makes some bounds check fire.
    // Never a crash, never a silent partial parse.
    for (std::size_t len = 1; len < full.size(); ++len) {
        std::vector<std::uint8_t> const trunc(full.begin(), full.begin() + len);
        DiagnosticReporter rep;
        auto got = pe::readRelocatableObject(trunc, *loaded.target, *loaded.format, rep);
        ASSERT_FALSE(got.has_value())
            << "truncation to " << len << " bytes must fail loud";
        EXPECT_GT(rep.errorCount(), 0u) << "a diagnostic must accompany the failure";
    }
}

// -- 3. Corruption red-pins ------------------------------------------

TEST(CoffObjectReader, NonzeroOptionalHeaderFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    auto obj = validObject(loaded);
    wr16(obj, kFhOptHdrSize, 0x00E0);   // a PE IMAGE (link OUTPUT), not a .obj
    DiagnosticReporter rep;
    EXPECT_FALSE(pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_UnsupportedBinaryFormat) saw = true;
    EXPECT_TRUE(saw)
        << "a non-zero SizeOfOptionalHeader must emit F_UnsupportedBinaryFormat";
}

TEST(CoffObjectReader, UnknownRelocNativeIdFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    auto obj = validObject(loaded);
    // Corrupt the `.text` reloc entry's Type field (u16 @ +8) to a value the
    // format schema does not declare, keeping SymbolTableIndex valid so the
    // nativeId check -- not the symbol-index check -- is what fires.
    std::uint32_t const relocPtr = rd32(obj, kSect0PtrReloc);
    ASSERT_GT(relocPtr, 0u);
    ASSERT_LT(static_cast<std::size_t>(relocPtr) + 10u, obj.size());
    wr16(obj, static_cast<std::size_t>(relocPtr) + 8u, 0xBEEF);   // undeclared nativeId
    DiagnosticReporter rep;
    EXPECT_FALSE(pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value())
        << "an undeclared reloc nativeId must not silently drop -- fail loud";
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_CorruptedBinary) saw = true;
    EXPECT_TRUE(saw);
}

TEST(CoffObjectReader, RelocSymbolIndexPastTableFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    auto obj = validObject(loaded);
    // Corrupt the `.text` reloc entry's SymbolTableIndex (u32 @ +4) past
    // NumberOfSymbols.
    std::uint32_t const relocPtr = rd32(obj, kSect0PtrReloc);
    ASSERT_GT(relocPtr, 0u);
    ASSERT_LT(static_cast<std::size_t>(relocPtr) + 10u, obj.size());
    wr32(obj, static_cast<std::size_t>(relocPtr) + 4u, 999u);
    DiagnosticReporter rep;
    EXPECT_FALSE(pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_CorruptedBinary) saw = true;
    EXPECT_TRUE(saw)
        << "a reloc SymbolTableIndex past NumberOfSymbols must fail loud";
}

TEST(CoffObjectReader, SectionNumberPastCountFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    auto obj = validObject(loaded);
    // Corrupt the DEFINED symbol `f`'s SectionNumber (i16 @ record+12) to an
    // ordinal past NumberOfSections. `f` is symbol record 0.
    std::uint32_t const symtabPtr = rd32(obj, kFhSymTabPtr);
    ASSERT_GT(symtabPtr, 0u);
    wr16(obj, static_cast<std::size_t>(symtabPtr) + 12u, 99u);   // > NumberOfSections
    DiagnosticReporter rep;
    EXPECT_FALSE(pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_CorruptedBinary) saw = true;
    EXPECT_TRUE(saw) << "a SectionNumber past NumberOfSections must fail loud";
}

// -- 4. Non-PE format schema -----------------------------------------

TEST(CoffObjectReader, NonPeFormatSchemaFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validObject(loaded);
    // An ELF format schema cannot parse a COFF object.
    auto elf = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(elf.has_value());
    DiagnosticReporter rep;
    EXPECT_FALSE(pe::readRelocatableObject(obj, *loaded.target, **elf, rep).has_value());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_UnsupportedBinaryFormat) saw = true;
    EXPECT_TRUE(saw) << "an ELF schema must fail loud F_UnsupportedBinaryFormat";
}

// -- 5. COFF function-type hint -> isData inference -------------------
//
// COFF carries a function-type hint on the IMAGE_SYMBOL (DTYPE_FUNCTION,
// 0x20 in the derived-type bits) -- UNLIKE Mach-O's nlist. The reader seeds an
// extern's isData from it in BOTH directions. `validObject`'s extern `g` is a
// FUNCTION import (isData=false), so the c170 writer emits DTYPE_FUNCTION on it:
// the reader reconstructs isData=false. Clearing the hint (Type=0) reconstructs
// isData=true. This pins the type-hint path red-on-disable both ways.
TEST(CoffObjectReader, ExternDtypeFunctionInfersFunction) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    auto obj = validObject(loaded);

    // Baseline: `g` is a function extern, the writer emitted DTYPE_FUNCTION ->
    // the reader infers a FUNCTION import (isData=false).
    {
        DiagnosticReporter rep;
        auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
        ASSERT_TRUE(got.has_value());
        auto const* eg = externNamed(*got, "g");
        ASSERT_NE(eg, nullptr);
        EXPECT_FALSE(eg->isData)
            << "the writer's DTYPE_FUNCTION hint -> a FUNCTION import (isData=false)";
    }

    // CLEAR the extern `g`'s IMAGE_SYMBOL Type (u16 @ record+14) to 0 and
    // re-read: with no derived-type hint the reader reconstructs isData=true
    // (data) -- the other direction of the same mask.
    std::size_t const gRec = findExternSymbolRecord(obj, "g");
    ASSERT_NE(gRec, 0u) << "must locate the extern `g` symbol record";
    wr16(obj, gRec + 14u, 0x0000);   // clear DTYPE_FUNCTION

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errors=" << rep.errorCount();
    auto const* eg = externNamed(*got, "g");
    ASSERT_NE(eg, nullptr);
    EXPECT_TRUE(eg->isData)
        << "a Type=0 extern reconstructs isData=true (data) -- no hint";
}

// ============================================================================
// TF-C53 (D-LK-COFF-READER-FOREIGN-OBJECT): read a REAL cl.exe/clang-cl `.obj`
// + a real multi-member `.lib` (cross-object COMDAT dedup). Two tiers:
//   * HERMETIC synthetic pins (run everywhere) -- a hand-rolled COFF builder
//     emits the shapes DSS's OWN writer never produces (COMDAT sections +
//     section-definition aux records + a kind-resolved section carrying a
//     reloc but no defining symbol), pinning Gate 1/2/3 red-on-disable with
//     NO toolchain, plus a 2-module cross-object COMDAT dedup at the
//     reader+merge level.
//   * NATIVE witnesses (`_WIN32`, GTEST_SKIP if cl.exe absent) -- a real
//     `cl /c /GS-` `.obj` wrapped in a real `lib.exe` `.lib`, static-linked
//     by the production `Program` driver -> a PE exec that RUNS -> exit 42;
//     the multi-member `.lib` COMDAT-dedup witness; and real-obj structural
//     reads of `cl /c /GS-` (+`/Gy`) objects.
// ============================================================================

namespace {

// -- A minimal hand-rolled COFF `.obj` builder ------------------------------
constexpr std::uint16_t kMachineAmd64 = 0x8664u;
constexpr std::uint32_t kScnText      = 0x60500020u;  // CODE|ALIGN16|EXEC|READ  (.text)
constexpr std::uint32_t kScnRData     = 0x40000040u;  // INITIALIZED_DATA|READ   (.rdata)
constexpr std::uint32_t kScnData      = 0xC0000040u;  // INIT_DATA|READ|WRITE    (.data)
constexpr std::uint32_t kScnXData     = 0x40300040u;  // INIT_DATA|ALIGN4|READ   (.xdata metadata)
constexpr std::uint32_t kScnLnkComdat = 0x00001000u;  // IMAGE_SCN_LNK_COMDAT
constexpr std::uint8_t  kSelNoDup = 1, kSelAny = 2, kSelAssoc = 5, kSelLargest = 6;
constexpr std::uint8_t  kClassExternal = 2, kClassStatic = 3;
constexpr std::uint16_t kDtypeFunction = 0x20u;
constexpr std::uint16_t kRelAddr64 = 1u;

struct BReloc { std::uint32_t va; std::string target; std::uint16_t type; };
struct BSec {
    std::string               name;    // <= 8 bytes (inline)
    std::uint32_t             chars = 0;
    std::vector<std::uint8_t> body;
    std::vector<BReloc>       relocs;
};
struct BSym {
    std::string   name;                 // <= 8 bytes (inline)
    std::uint32_t value   = 0;
    std::uint16_t sectNum = 0;
    std::uint16_t type    = 0;
    std::uint8_t  storage = kClassExternal;
    std::optional<std::uint8_t> auxSelection;   // set -> emit a section-def aux
};

void emitU16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}
void emitU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
}
void emitName8(std::vector<std::uint8_t>& b, std::string const& n) {
    for (std::size_t i = 0; i < 8u; ++i)
        b.push_back(i < n.size() ? static_cast<std::uint8_t>(n[i]) : 0u);
}

// Assemble a COFF `.obj`. Symbol order is preserved; a symbol carrying an
// auxSelection emits a section-definition aux (Selection @ +14) right after
// it. Reloc targets resolve by symbol NAME to the FINAL symtab index (aux
// slots shift indices). All names inline (<= 8) -> the string table is the
// 4-byte size prefix only.
[[nodiscard]] std::vector<std::uint8_t>
buildCoff(std::vector<BSec> const& secs, std::vector<BSym> const& syms) {
    std::unordered_map<std::string, std::uint32_t> symIndex;
    std::uint32_t slot = 0;
    for (auto const& s : syms) {
        if (!s.name.empty()) symIndex.emplace(s.name, slot);
        slot += 1u + (s.auxSelection.has_value() ? 1u : 0u);
    }
    std::uint32_t const numSymbols  = slot;
    std::uint16_t const numSections = static_cast<std::uint16_t>(secs.size());

    std::size_t cursor = 20u + 40u * static_cast<std::size_t>(numSections);
    std::vector<std::uint32_t> bodyOff(secs.size()), relocOff(secs.size());
    for (std::size_t i = 0; i < secs.size(); ++i) {
        bodyOff[i] = static_cast<std::uint32_t>(cursor);
        cursor += secs[i].body.size();
    }
    for (std::size_t i = 0; i < secs.size(); ++i) {
        relocOff[i] = secs[i].relocs.empty() ? 0u : static_cast<std::uint32_t>(cursor);
        cursor += secs[i].relocs.size() * 10u;
    }
    std::uint32_t const symTabPtr = static_cast<std::uint32_t>(cursor);

    std::vector<std::uint8_t> out;
    emitU16(out, kMachineAmd64);
    emitU16(out, numSections);
    emitU32(out, 0u);            // TimeDateStamp
    emitU32(out, symTabPtr);
    emitU32(out, numSymbols);
    emitU16(out, 0u);            // SizeOfOptionalHeader (relocatable)
    emitU16(out, 0u);            // Characteristics
    for (std::size_t i = 0; i < secs.size(); ++i) {
        emitName8(out, secs[i].name);
        emitU32(out, 0u);                                                 // VirtualSize
        emitU32(out, 0u);                                                 // VirtualAddress
        emitU32(out, static_cast<std::uint32_t>(secs[i].body.size()));    // SizeOfRawData
        emitU32(out, secs[i].body.empty() ? 0u : bodyOff[i]);             // PointerToRawData
        emitU32(out, relocOff[i]);                                        // PointerToRelocations
        emitU32(out, 0u);                                                 // PointerToLinenumbers
        emitU16(out, static_cast<std::uint16_t>(secs[i].relocs.size()));  // NumberOfRelocations
        emitU16(out, 0u);                                                 // NumberOfLinenumbers
        emitU32(out, secs[i].chars);                                      // Characteristics
    }
    for (auto const& s : secs) out.insert(out.end(), s.body.begin(), s.body.end());
    for (auto const& s : secs) {
        for (auto const& r : s.relocs) {
            emitU32(out, r.va);
            auto const it = symIndex.find(r.target);
            emitU32(out, it == symIndex.end() ? 0u : it->second);
            emitU16(out, r.type);
        }
    }
    for (auto const& s : syms) {
        emitName8(out, s.name);
        emitU32(out, s.value);
        emitU16(out, s.sectNum);
        emitU16(out, s.type);
        out.push_back(s.storage);
        out.push_back(s.auxSelection.has_value() ? 1u : 0u);
        if (s.auxSelection.has_value()) {
            std::array<std::uint8_t, 18> aux{};
            aux[14] = *s.auxSelection;   // Selection byte (section-def aux, format 5)
            out.insert(out.end(), aux.begin(), aux.end());
        }
    }
    emitU32(out, 4u);            // string table: size prefix only (all names inline)
    return out;
}

// The reconstructed binding of a defined symbol (by name); nullopt if unknown.
[[nodiscard]] std::optional<SymbolBinding>
bindingOf(AssembledModule const& m, std::string const& name) {
    for (auto const& s : m.symbols) if (s.name == name) return s.binding;
    return std::nullopt;
}
[[nodiscard]] bool sawCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) if (d.code == code) return true;
    return false;
}
[[nodiscard]] bool sawDetail(DiagnosticReporter const& rep, std::string_view needle) {
    for (auto const& d : rep.all())
        if (d.actual.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// -- Gate 1 + Gate 3 (NODUPLICATES): a `.text$mn` COMDAT function ------------
TEST(CoffForeignObject, TextDollarComdatNoDuplicatesReconstructsGlobalFunction) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> const foo = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3}; // mov eax,42; ret
    auto const obj = buildCoff(
        {BSec{".text$mn", kScnText | kScnLnkComdat, foo, {}}},
        {BSym{".text$mn", 0, 1, 0, kClassStatic, kSelNoDup},
         BSym{"foo", 0, 1, kDtypeFunction, kClassExternal, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value())
        << "a `.text$mn` COMDAT NODUPLICATES function must reconstruct (Gate 1 "
           "$-name + Gate 3); errs=" << rep.errorCount();
    auto const* rFoo = funcNamed(*got, "foo");
    ASSERT_NE(rFoo, nullptr) << "the $-grouped `.text$mn` must resolve to Text (Gate 1)";
    EXPECT_EQ(rFoo->bytes, foo);
    auto const b = bindingOf(*got, "foo");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, SymbolBinding::Global)
        << "NODUPLICATES(1) keeps the symbol STRONG/Global (a duplicate is an error)";
}

// -- Gate 3 (ANY): a `.data` selectany COMDAT datum lifts to Weak -----------
TEST(CoffForeignObject, DataComdatAnyLiftsWeak) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> const wbytes = {42, 0, 0, 0};
    auto const obj = buildCoff(
        {BSec{".data", kScnData | kScnLnkComdat, wbytes, {}}},
        {BSym{".data", 0, 1, 0, kClassStatic, kSelAny},
         BSym{"W", 0, 1, 0, kClassExternal, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    auto const* dW = dataNamed(*got, "W");
    ASSERT_NE(dW, nullptr);
    EXPECT_EQ(dW->section, DataSectionKind::Data);
    EXPECT_EQ(dW->bytes, wbytes);
    auto const b = bindingOf(*got, "W");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, SymbolBinding::Weak)
        << "ANY(2)/SAME_SIZE(3)/EXACT_MATCH(4) lift to Weak so the all-weak merge "
           "dedups duplicates -- red-on-disable vs the pre-TF-C53 hardcoded Global";
}

// -- Gate 3: LARGEST / ASSOCIATIVE on a kind-resolved COMDAT -> FAIL LOUD ----
TEST(CoffForeignObject, ComdatLargestOnDataFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".data", kScnData | kScnLnkComdat, {42, 0, 0, 0}, {}}},
        {BSym{".data", 0, 1, 0, kClassStatic, kSelLargest},
         BSym{"W", 0, 1, 0, kClassExternal, std::nullopt}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value())
        << "LARGEST(6) on a code/data COMDAT must fail loud (silent wrong-size risk)";
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_CorruptedBinary));
    EXPECT_TRUE(sawDetail(rep, "D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION"));
}

TEST(CoffForeignObject, ComdatAssociativeOnDataFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".data", kScnData | kScnLnkComdat, {42, 0, 0, 0}, {}}},
        {BSym{".data", 0, 1, 0, kClassStatic, kSelAssoc},
         BSym{"W", 0, 1, 0, kClassExternal, std::nullopt}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value())
        << "ASSOCIATIVE(5) on a kind-resolved code/data COMDAT must fail loud (unmodeled)";
    EXPECT_TRUE(sawDetail(rep, "D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION"));
}

// -- Gate 3: a COMDAT section with NO section-def aux -> FAIL LOUD -----------
TEST(CoffForeignObject, ComdatSectionMissingSectionDefAuxFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    // A COMDAT `.data` whose only symbol is the External datum -- NO STATIC
    // section symbol carrying the aux -> the selection cannot be read -> fail
    // loud (never default a selection).
    auto const obj = buildCoff(
        {BSec{".data", kScnData | kScnLnkComdat, {42, 0, 0, 0}, {}}},
        {BSym{"W", 0, 1, 0, kClassExternal, std::nullopt}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawDetail(rep, "D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION"));
}

// -- Gate 2 red-on-disable: a KIND-RESOLVED section, reloc, no atom -> LOUD --
TEST(CoffForeignObject, KindResolvedSectionWithRelocButNoAtomFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    // A `.rdata` (kind resolves via reloc-presence -> RelRoConst) carrying ONE
    // ADDR64 reloc but NO defining symbol -> no atom. The skip is KIND-gated,
    // so a resolved-kind section must FAIL LOUD (revert Gate 2 -> it would
    // WRONGLY skip a real code/data section's relocations = a silent drop).
    auto const obj = buildCoff(
        {BSec{".rdata", kScnRData, std::vector<std::uint8_t>(8, 0),
              {BReloc{0u, "g", kRelAddr64}}}},
        {BSym{"g", 0, 0, 0, kClassExternal, std::nullopt}});   // g: undefined extern
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value())
        << "a kind-RESOLVED `.rdata` with a reloc but no atom must fail loud, not "
           "skip (Gate 2 is kind-gated, not atom-gated)";
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_CorruptedBinary));
    EXPECT_TRUE(sawDetail(rep, "D-LK-COFF-READER-ANONYMOUS-GAP-ATOMS"));
}

// -- Gate 2 skip: an UNMODELED metadata section (+ its reloc) is skipped -----
TEST(CoffForeignObject, UnmodeledMetadataSectionWithRelocSkipped) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    // A `.xdata` (base name absent from the schema -> kind nullopt) carrying a
    // reloc but no atom -> SKIPPED whole (metadata), reads cleanly.
    auto const obj = buildCoff(
        {BSec{".xdata", kScnXData, std::vector<std::uint8_t>(8, 0),
              {BReloc{0u, "g", kRelAddr64}}}},
        {BSym{"g", 0, 0, 0, kClassExternal, std::nullopt}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value())
        << "an unmodeled `.xdata` metadata section (+ its reloc) must be skipped, "
           "not fail loud; errs=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_NE(externNamed(*got, "g"), nullptr) << "the extern g is still decoded";
}

// -- Cross-object COMDAT dedup at the reader+merge level (hermetic) ----------
//
// TWO synthetic objects, each with a DISTINCT Global function (alpha / beta)
// PLUS the SAME `.data` COMDAT ANY datum `shared_w`. Read both (distinct
// cuIds), then MERGE via linker::link: the two Weak `shared_w` bodies dedup
// (lowest key wins, the shadow drops) with ZERO merge change -> no
// K_SymbolRedefinedAcrossUnits. RED-ON-DISABLE: revert Gate 3's weak-lift ->
// both `shared_w` stay Global -> two strong defs -> K_SymbolRedefinedAcrossUnits.
TEST(CoffForeignObject, CrossObjectComdatAnyDedupsInMerge) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    auto makeMember = [](std::string const& fn, std::uint8_t retImm) {
        std::vector<std::uint8_t> const body = {0xB8, retImm, 0x00, 0x00, 0x00, 0xC3};
        return buildCoff(
            {BSec{".data", kScnData | kScnLnkComdat, {1, 0, 0, 0}, {}},
             BSec{".text$mn", kScnText, body, {}}},
            {BSym{".data", 0, 1, 0, kClassStatic, kSelAny},
             BSym{"shared_w", 0, 1, 0, kClassExternal, std::nullopt},
             BSym{fn, 0, 2, kDtypeFunction, kClassExternal, std::nullopt}});
    };
    auto const objA = makeMember("alpha", 20);
    auto const objB = makeMember("beta", 22);

    DiagnosticReporter repA, repB;
    auto modA = pe::readRelocatableObject(objA, *loaded.target, *loaded.format, repA,
                                          CompilationUnitId{1});
    auto modB = pe::readRelocatableObject(objB, *loaded.target, *loaded.format, repB,
                                          CompilationUnitId{2});
    ASSERT_TRUE(modA.has_value() && modB.has_value());
    // Each member's shared_w reconstructs WEAK (the direct red-on-disable pin).
    EXPECT_EQ(bindingOf(*modA, "shared_w").value_or(SymbolBinding::Global),
              SymbolBinding::Weak);
    EXPECT_EQ(bindingOf(*modB, "shared_w").value_or(SymbolBinding::Global),
              SymbolBinding::Weak);

    std::array<AssembledModule, 2> const mods{*modA, *modB};
    DiagnosticReporter linkRep;
    auto const image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()},
        *loaded.target, *loaded.format, linkRep);
    EXPECT_FALSE(sawCode(linkRep, DiagnosticCode::K_SymbolRedefinedAcrossUnits))
        << "the two Weak `shared_w` COMDAT bodies must DEDUP (no strong-vs-strong "
           "conflict) -- the all-weak merge keeps one + drops the shadow, ZERO "
           "merge change";
}

// ============================================================================
// NATIVE witnesses (`_WIN32`; GTEST_SKIP when no cl.exe/lib.exe toolchain).
// ============================================================================

namespace {

#if defined(_WIN32)
// Locate a cl.exe/lib.exe toolchain via vswhere -> vcvars64. Returns a functor
// that runs a command line under the MSVC environment in `work`, or nullopt if
// no toolchain is present (the native witnesses then GTEST_SKIP). Mirrors the
// vcvars64 shell-out in tests/core/test_bitfield_abi_conformance.cpp.
struct MsvcEnv {
    std::filesystem::path vcvars;
    std::filesystem::path work;
    [[nodiscard]] bool run(std::string const& cmdline) const {
        auto const bat = work / "dss_c53_build.bat";
        {
            std::ofstream b{bat};
            b << "@echo off\r\n"
              << "call \"" << vcvars.string() << "\" >nul 2>&1\r\n"
              << "cd /d \"" << work.string() << "\"\r\n"
              << cmdline << " >nul 2>&1\r\n";
        }
        std::string const sys = "\"\"" + bat.string() + "\"\"";
        return std::system(sys.c_str()) == 0;
    }
};

[[nodiscard]] std::optional<MsvcEnv> findMsvc(std::filesystem::path const& work) {
    std::filesystem::path const vswhere =
        "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe";
    std::error_code ec;
    if (!std::filesystem::exists(vswhere, ec)) return std::nullopt;
    auto const outTxt = work / "vsinstall.txt";
    std::string const q =
        "\"\"" + vswhere.string() + "\" -latest -products * "
        "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
        "-property installationPath > \"" + outTxt.string() + "\"\"";
    if (std::system(q.c_str()) != 0) return std::nullopt;
    std::ifstream vin{outTxt};
    std::string vsPath;
    std::getline(vin, vsPath);
    while (!vsPath.empty() && (vsPath.back() == '\r' || vsPath.back() == '\n'))
        vsPath.pop_back();
    if (vsPath.empty()) return std::nullopt;
    std::filesystem::path const vcvars =
        std::filesystem::path{vsPath} / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    if (!std::filesystem::exists(vcvars, ec)) return std::nullopt;
    return MsvcEnv{vcvars, work};
}

[[nodiscard]] std::vector<std::uint8_t> readFile(std::filesystem::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
#endif  // _WIN32

}  // namespace

// -- Structural: a real `cl /c /GS-` `.obj` reconstructs (Gate 1 + Gate 2) ---
TEST(CoffForeignObjectNative, RealClObjReconstructsFunctionAndSkipsMetadata) {
#if !defined(_WIN32)
    GTEST_SKIP() << "reads a freshly-compiled cl.exe `.obj`; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-foreign"};
    auto const dir = scratch.path();
    auto const msvc = findMsvc(dir);
    if (!msvc) GTEST_SKIP() << "no cl.exe toolchain (vswhere/vcvars64 absent)";
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    // A NON-leaf function -> cl emits `.text$mn` (function, a REL32 to helper) +
    // `.pdata`/`.xdata` (unwind metadata, `.pdata` carries relocations) +
    // `.drectve`/`.debug$S`/`.chks64` + `@feat.00`. The reader must reconstruct
    // the function ($-name, Gate 1) and SKIP `.pdata`'s relocations (Gate 2),
    // tolerating @feat.00.
    { std::ofstream f{dir / "bar.c"};
      f << "int helper(int*p);\n"
           "int bar(int x){ int b[16]; for(int i=0;i<16;++i) b[i]=x+i; return helper(b); }\n"; }
    ASSERT_TRUE(msvc->run("cl /nologo /c /GS- bar.c")) << "cl must compile bar.c";
    ASSERT_TRUE(std::filesystem::exists(dir / "bar.obj"));

    auto const bytes = readFile(dir / "bar.obj");
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(bytes, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value())
        << "a real cl.exe `.obj` must reconstruct (Gate 1 $-name + Gate 2 metadata "
           "skip + @feat.00 tolerated); errs=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    auto const* rBar = funcNamed(*got, "bar");
    ASSERT_NE(rBar, nullptr) << "the `.text$mn` function `bar` must reconstruct";
    EXPECT_FALSE(rBar->bytes.empty());
    EXPECT_NE(externNamed(*got, "helper"), nullptr)
        << "the `.text` REL32 to the undefined `helper` must reconstruct as an extern";
#endif
}

// -- Structural: a `/Gy` `.obj` (COMDAT fn + ASSOCIATIVE .pdata/.xdata) reads -
TEST(CoffForeignObjectNative, RealGyObjComdatFunctionAssociativeMetadataSkipped) {
#if !defined(_WIN32)
    GTEST_SKIP() << "Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-foreign"};
    auto const dir = scratch.path();
    auto const msvc = findMsvc(dir);
    if (!msvc) GTEST_SKIP() << "no cl.exe toolchain";
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    // `/Gy` makes `barg` a COMDAT `.text$mn` (NODUPLICATES -> Global) AND its
    // `.pdata`/`.xdata` ASSOCIATIVE(5) COMDAT sections (cl.exe-witnessed). Gate 3
    // is gated on resolved-kind, so the associative METADATA is skipped by Gate 2
    // (never fail-loud) -> a real `/Gy` object reads cleanly.
    { std::ofstream f{dir / "barg.c"};
      f << "int helper(int*p);\n"
           "int barg(int x){ int b[16]; for(int i=0;i<16;++i) b[i]=x+i; return helper(b); }\n"; }
    ASSERT_TRUE(msvc->run("cl /nologo /c /GS- /Gy barg.c"));
    ASSERT_TRUE(std::filesystem::exists(dir / "barg.obj"));

    auto const bytes = readFile(dir / "barg.obj");
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(bytes, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value())
        << "a `/Gy` object (COMDAT fn + ASSOCIATIVE `.pdata`/`.xdata`) must read "
           "cleanly (the kind-gate skips associative metadata); errs=" << rep.errorCount();
    auto const* rBar = funcNamed(*got, "barg");
    ASSERT_NE(rBar, nullptr);
    auto const b = bindingOf(*got, "barg");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, SymbolBinding::Global)
        << "a `/Gy` C function COMDAT is NODUPLICATES(1) -> Global (cl.exe-witnessed)";
#endif
}

// -- THE WITNESS: single real cl.exe `.obj` -> `.lib` -> link -> RUN -> 42 ----
TEST(CoffForeignObjectNative, SingleClObjStaticLinkExitsFortyTwo) {
#if !defined(_WIN32)
    GTEST_SKIP() << "the native cl.exe COFF witness runs on Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-foreign"};
    auto const dir = scratch.path();
    auto const msvc = findMsvc(dir);
    if (!msvc) GTEST_SKIP() << "no cl.exe/lib.exe toolchain";

    { std::ofstream f{dir / "foo.c"}; f << "int foo(void){ return 42; }\n"; }
    ASSERT_TRUE(msvc->run("cl /nologo /c /GS- foo.c")) << "cl must compile foo.c";
    ASSERT_TRUE(std::filesystem::exists(dir / "foo.obj"));
    ASSERT_TRUE(msvc->run("lib /nologo /out:foo.lib foo.obj")) << "lib must wrap foo.obj";
    ASSERT_TRUE(std::filesystem::exists(dir / "foo.lib"));

    { std::ofstream m{dir / "main.c"};
      m << "extern int foo(void);\nint main(void){ return foo(); }\n"; }

    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries(std::vector<std::filesystem::path>{dir / "foo.lib"});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{(dir / "main.c").string()}, "c-subset",
        std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    ASSERT_EQ(rc, 0) << "static-link against the real cl.exe `.lib` must succeed; errs="
                     << rep.errorCount();
    auto const exe = dir / "main.exe";
    ASSERT_TRUE(std::filesystem::exists(exe));

    auto const r = test_support::runBinary(exe, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE witness: exit 42 = foo() pulled from a real cl.exe `.obj` (wrapped in "
           "a real lib.exe `.lib`), read by the COFF foreign-object reader, merged, run";
#endif
}

// -- THE WITNESS: multi-member `.lib` with cross-object COMDAT dedup -> 42 ----
TEST(CoffForeignObjectNative, MultiMemberComdatDedupExitsFortyTwo) {
#if !defined(_WIN32)
    GTEST_SKIP() << "the native multi-member COFF witness runs on Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-foreign"};
    auto const dir = scratch.path();
    auto const msvc = findMsvc(dir);
    if (!msvc) GTEST_SKIP() << "no cl.exe/lib.exe toolchain";

    // a.c + b.c each define a DISTINCT function AND the SAME selectany COMDAT
    // datum `shared_w` (ANY selection). main references BOTH functions -> BOTH
    // members are pulled from the armap -> the two `shared_w` bodies dedup.
    { std::ofstream f{dir / "a.c"};
      f << "__declspec(selectany) int shared_w = 1;\nint alpha(void){ return 20; }\n"; }
    { std::ofstream f{dir / "b.c"};
      f << "__declspec(selectany) int shared_w = 1;\nint beta(void){ return 22; }\n"; }
    ASSERT_TRUE(msvc->run("cl /nologo /c /GS- a.c"));
    ASSERT_TRUE(msvc->run("cl /nologo /c /GS- b.c"));
    ASSERT_TRUE(msvc->run("lib /nologo /out:ab.lib a.obj b.obj"));
    ASSERT_TRUE(std::filesystem::exists(dir / "ab.lib"));

    { std::ofstream m{dir / "main.c"};
      m << "extern int alpha(void);\nextern int beta(void);\n"
           "int main(void){ return alpha() + beta(); }\n"; }

    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries(std::vector<std::filesystem::path>{dir / "ab.lib"});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{(dir / "main.c").string()}, "c-subset",
        std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    ASSERT_EQ(rc, 0) << "both members pull + the shared_w COMDAT dedups; errs="
                     << rep.errorCount();
    auto const exe = dir / "main.exe";
    ASSERT_TRUE(std::filesystem::exists(exe));

    auto const r = test_support::runBinary(exe, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "exit 42 = alpha()+beta() (20+22) with the duplicate selectany `shared_w` "
           "COMDAT deduped across the two `.lib` members (revert the Gate 3 weak-lift "
           "-> both shared_w Global -> K_SymbolRedefinedAcrossUnits -> rc != 0)";
#endif
}
