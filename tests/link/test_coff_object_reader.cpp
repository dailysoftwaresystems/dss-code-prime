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
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/pe.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

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
