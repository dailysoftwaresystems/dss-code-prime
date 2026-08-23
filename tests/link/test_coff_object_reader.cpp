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
#include "ffi/binary_readers/ar_reader.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/pe.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "repo_root.hpp"   // the ONE test-side repo/config-root resolver
#include "run_binary.hpp"
#include "scratch_dir.hpp"

// The native-witness skip-vs-fail vocabulary, shared with the two ABI conformance
// witnesses under tests/core (D-TEST-NATIVE-ORACLE-INERT-ON-POSIX). Spelled relative
// because only `tests/test_support` is on this target's include path; the header's
// natural long-term home IS `tests/test_support/`, and moving it there would drop this
// `../` — deliberately left for whoever owns that shared directory.
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>   // the "shipped file PLUS one row" alias fixture

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dss;
namespace native_probe = dss::test_support::native_probe;

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

// -- 5b. A DECLARED `isCall` ROLE UPGRADES A HINT-LESS EXTERN ---------
//
// D-LK-MACHO-ISDATA-NO-CALL-SIGNAL, the COFF end of it. This reader ALSO reads
// the format row's declared `isCall` role, and the leg is not decoration:
// COFF's type hint is authoritative when it is PRESENT, and a foreign object
// whose extern carries no derived type (the case the test above pins as
// reconstructing DATA) has nothing else to go on. A relocation that can only
// target executable code is exactly that missing evidence.
//
// ⚠ NO SHIPPED PE DOCUMENT DECLARES `isCall`, AND NONE SHOULD. ✔MEASURED
// 2026-08-20 over the four shipped pe64 documents: they declare
// IMAGE_REL_AMD64_REL32 / ADDR64 / ADDR32 / SECREL and not one of them is
// branch-only -- REL32 is both the `call` displacement and the `lea rip+d`
// data displacement -- so declaring the role on any of them would re-commit
// the very conflation this anchor removed, in a new field. PE also declares no
// `pltNativeId`, so `callSignalNativeIds` is EMPTY on every shipped PE format
// today and this leg would otherwise be untested code.
//
// ★ THE FIXTURE IS THEREFORE A FICTION, DELIBERATELY AND VISIBLY. It is the
// shipped document PLUS one key, loaded under a name no format claims, and it
// stands in for the branch-only PE relocation that exists in the PE/COFF spec
// but not in any target DSS ships yet (IMAGE_REL_ARM64_BRANCH26). It asserts
// what the READER does with a declared role; it asserts nothing about REL32.
TEST(CoffObjectReader, DeclaredIsCallRoleUpgradesAnExternWithNoTypeHint) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validObject(loaded);

    // Strip the hint, so the ONLY remaining evidence is the relocation.
    std::size_t const gRec = findExternSymbolRecord(obj, "g");
    ASSERT_NE(gRec, 0u);
    wr16(obj, gRec + 14u, 0x0000);

    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    std::string text;
    {
        std::ifstream in{*root / "src" / "dss-config" / "object-formats"
                             / "pe64-x86_64-windows.format.json",
                         std::ios::binary};
        ASSERT_TRUE(in.good());
        text.assign(std::istreambuf_iterator<char>{in},
                    std::istreambuf_iterator<char>{});
    }
    ASSERT_FALSE(text.empty());
    ASSERT_EQ(text.find("\"isCall\""), std::string::npos)
        << "the shipped PE document must NOT declare a call role -- if it does, "
           "the value is wrong (COFF x86_64 has no branch-only relocation) and "
           "this fixture is no longer the 'plus one key' it claims to be";

    // CONTROL: unmodified, the reader reconstructs the hint-less extern as
    // DATA. Without this the assertion below could not be attributed to the
    // added key.
    {
        auto control = ObjectFormatSchema::loadFromText(text, "pe64-x86_64-windows");
        ASSERT_TRUE(control.has_value());
        DiagnosticReporter rep;
        auto got = pe::readRelocatableObject(obj, *loaded.target, **control, rep);
        ASSERT_TRUE(got.has_value()) << "errors=" << rep.errorCount();
        auto const* eg = externNamed(*got, "g");
        ASSERT_NE(eg, nullptr);
        EXPECT_TRUE(eg->isData)
            << "control: no type hint and no declared call role -> DATA";
    }

    // The SAME document plus exactly one key on the row the object's
    // relocation actually uses.
    auto const at = text.find("\"name\": \"IMAGE_REL_AMD64_REL32\"");
    ASSERT_NE(at, std::string::npos)
        << "the REL32 row moved or was renamed -- re-derive this fixture";
    std::string spliced = text;
    spliced.insert(at, "\"isCall\": true,\n      ");

    auto fiction = ObjectFormatSchema::loadFromText(spliced, "pe64-fictional-branch-only");
    ASSERT_TRUE(fiction.has_value()) << "the spliced document must still load";

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, **fiction, rep);
    ASSERT_TRUE(got.has_value()) << "errors=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    auto const* eg = externNamed(*got, "g");
    ASSERT_NE(eg, nullptr);
    EXPECT_FALSE(eg->isData)
        << "the extern is reached by a relocation the FORMAT declares can only "
           "target executable code, so it is a FUNCTION even with no type hint "
           "-- the COFF reader reads the declared role, it does not re-derive "
           "it from the target's arithmetic formula";
}

// AN EMISSION ALIAS IS A FORMAT-AGNOSTIC ESCAPE HATCH, and this reader used to
// be the second of two that could not honour it.
//
// `emitOnly` states that one WIRE type carries two DSS patch-site semantics,
// so the emitter can reach it through a second `kind` while the DECODER keeps
// exactly one answer per wire id. `elf_object_reader.cpp` excluded alias rows
// from its reverse map; this reader's copy of that loop did NOT, and the
// difference was invisible because no shipped PE document declares one.
//
// It was never a miscompile, and this pin does not claim it was. Every valid
// document has UNIQUE `kind`s (`validateRelocationsTable`), so an alias always
// carries a different kind from the row owning its wire id, so the old loop
// would have hit its own ambiguity refusal -- LOUD, and refusing every object
// of the format. What was missing was the CAPABILITY, on two of three formats.
//
// The fix is not a second `if (r.emitOnly) continue;`: it is that no reader
// owns that loop any more (`ObjectFormatSchema::relocationDecodeTable`). This
// pin is therefore also the COFF witness that the shared builder is wired in.
//
// RED-ON-DISABLE: delete the `if (r.emitOnly) continue;` in
// `relocationDecodeTable` and the spliced read below is refused with
// "ambiguous reverse map" (this test + the Mach-O and ELF twins go red).
TEST(CoffObjectReader, EmitOnlyAliasIsHonouredNotRefused) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validObject(loaded);

    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    std::string text;
    {
        std::ifstream in{*root / "src" / "dss-config" / "object-formats"
                             / "pe64-x86_64-windows.format.json",
                         std::ios::binary};
        ASSERT_TRUE(in.good());
        text.assign(std::istreambuf_iterator<char>{in},
                    std::istreambuf_iterator<char>{});
    }
    ASSERT_FALSE(text.empty());
    ASSERT_EQ(text.find("\"emitOnly\""), std::string::npos)
        << "no shipped PE document declares an emission alias -- if one now "
           "does, this fixture is no longer the 'plus one row' it claims to "
           "be, and the gap it pins was no longer latent";

    // CONTROL: unmodified, these bytes decode to kind 1. Without this the
    // assertion below could not be attributed to the added row.
    {
        auto control = ObjectFormatSchema::loadFromText(text,
                                                        "pe64-x86_64-windows");
        ASSERT_TRUE(control.has_value());
        DiagnosticReporter rep;
        auto got = pe::readRelocatableObject(obj, *loaded.target, **control, rep);
        ASSERT_TRUE(got.has_value()) << "errors=" << rep.errorCount();
        ASSERT_EQ(got->functions.size(), 1u);
        ASSERT_EQ(got->functions[0].relocations.size(), 1u);
        EXPECT_EQ(got->functions[0].relocations[0].kind.v, 1u);
    }

    // The SAME document plus exactly one row: an alias of whatever wire id the
    // kind-1 row owns, carrying the first kind nothing else claims. Both
    // numbers are DERIVED from the document -- hardcoding either would go
    // stale silently if the shipped rows were renumbered.
    nlohmann::json doc = nlohmann::json::parse(text);
    std::uint32_t ownerNativeId = 0;
    std::uint32_t aliasKind     = 0;
    bool          ownerFound    = false;
    {
        std::vector<std::uint32_t> taken;
        for (auto const& row : doc.at("relocations")) {
            auto const k = row.at("kind").get<std::uint32_t>();
            taken.push_back(k);
            if (k == 1u) {
                ownerNativeId = row.at("nativeId").get<std::uint32_t>();
                ownerFound    = true;
            }
        }
        ASSERT_TRUE(ownerFound)
            << "the kind-1 row moved or was renumbered -- re-derive this fixture";
        for (std::uint32_t k = 1; k < 64u && aliasKind == 0u; ++k) {
            if (std::ranges::find(taken, k) == taken.end()) aliasKind = k;
        }
        ASSERT_NE(aliasKind, 0u);
    }
    doc.at("relocations").push_back(nlohmann::json{
        {"name", "IMAGE_REL_AMD64_REL32_EMIT_ALIAS"},
        {"kind", aliasKind},
        {"nativeId", ownerNativeId},
        {"emitOnly", true}});

    auto aliased = ObjectFormatSchema::loadFromText(doc.dump(),
                                                     "pe64-x86_64-alias");
    ASSERT_TRUE(aliased.has_value())
        << "an emission alias is a SCHEMA-level shape, not an ELF one -- "
           "`validate()` must accept it for every format";

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, **aliased, rep);
    ASSERT_TRUE(got.has_value())
        << "the alias must not enter the reverse map: if it does, the wire id "
           "it shares maps to two kinds and EVERY object of this format is "
           "refused as ambiguous. errors=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(got->functions.size(), 1u);
    ASSERT_EQ(got->functions[0].relocations.size(), 1u);
    EXPECT_EQ(got->functions[0].relocations[0].kind.v, 1u)
        << "the alias must not DISPLACE the row that owns the wire id either "
           "-- decoding has to keep yielding the owning kind, which is what a "
           "'last row wins' map would get wrong (the alias is appended LAST on "
           "purpose)";
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
    // SizeOfRawData when it must DIFFER from the body length -- the ONE real
    // shape that needs it. ✔MEASURED on cl.exe 14.51.36231 and mingw gcc
    // 13.2.0: a `.bss` declares `size of raw data` = 0x10 with `file pointer to
    // raw data` = 0, i.e. an extent with no file bytes behind it. Deriving the
    // size from `body` alone cannot express that, and a `.bss` of size 0 makes
    // any reservation assertion vacuous.
    std::optional<std::uint32_t> rawSizeOverride;
};
// Auxiliary Format 3 (PE/COFF 5.5.3) on a WEAK_EXTERNAL record. `defaultName`
// resolves to the FINAL symtab index the same way a relocation target does, so a
// fixture never hand-computes an index that aux slots shift; `rawTagIndex`
// overrides it for the corruption pins that must name an index ON PURPOSE.
struct BWeakAux {
    std::string                  defaultName;
    std::uint32_t                characteristics = 1;  // SEARCH_NOLIBRARY
    std::optional<std::uint32_t> rawTagIndex;
};
struct BSym {
    std::string   name;                 // <= 8 bytes (inline)
    std::uint32_t value   = 0;
    std::uint16_t sectNum = 0;
    std::uint16_t type    = 0;
    std::uint8_t  storage = kClassExternal;
    std::optional<std::uint8_t> auxSelection;   // set -> emit a section-def aux
    std::optional<BWeakAux>     auxWeakExtern;  // set -> emit a format-3 aux
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
    auto auxCount = [](BSym const& s) -> std::uint32_t {
        return (s.auxSelection.has_value() ? 1u : 0u)
             + (s.auxWeakExtern.has_value() ? 1u : 0u);
    };
    std::unordered_map<std::string, std::uint32_t> symIndex;
    std::uint32_t slot = 0;
    for (auto const& s : syms) {
        if (!s.name.empty()) symIndex.emplace(s.name, slot);
        slot += 1u + auxCount(s);
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
        emitU32(out, secs[i].rawSizeOverride.value_or(
                         static_cast<std::uint32_t>(secs[i].body.size())));  // SizeOfRawData
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
        out.push_back(static_cast<std::uint8_t>(auxCount(s)));
        if (s.auxSelection.has_value()) {
            std::array<std::uint8_t, 18> aux{};
            aux[14] = *s.auxSelection;   // Selection byte (section-def aux, format 5)
            out.insert(out.end(), aux.begin(), aux.end());
        }
        if (s.auxWeakExtern.has_value()) {
            // Auxiliary Format 3: [0] u32 TagIndex, [4] u32 Characteristics,
            // [8] 10 unused bytes.
            auto const& w = *s.auxWeakExtern;
            std::uint32_t tag = 0;
            if (w.rawTagIndex.has_value()) {
                tag = *w.rawTagIndex;
            } else {
                auto const it = symIndex.find(w.defaultName);
                tag = (it == symIndex.end()) ? 0u : it->second;
            }
            std::vector<std::uint8_t> aux;
            emitU32(aux, tag);
            emitU32(aux, w.characteristics);
            aux.resize(18u, 0u);
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
// ⓘ THE DATA HALF OF THIS PIN MOVED. It used to use an anonymous `.rdata`,
// because that was a kind-resolved section that could reach Gate 2 with no atom.
// The gap-atom pass now mints an atom for exactly those bytes, so `.rdata` can
// no longer get there -- and the arm below proves that rather than deleting it.
// A TEXT section still can, and deliberately: a `.text` gap is inter-function
// ALIGNMENT PADDING, so fabricating a code atom from it would give a corrupt
// code reference somewhere to land instead of failing loud.
TEST(CoffForeignObject, KindResolvedTextSectionWithRelocButNoAtomFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    // A `.text` carrying ONE reloc but NO defining symbol -> no atom, and no gap
    // atom either (Text never gap-fills). The skip is KIND-gated, so a
    // resolved-kind section must FAIL LOUD (revert Gate 2 -> it would WRONGLY
    // skip a real code section's relocations = a silent drop).
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(8, 0x90u),
              {BReloc{0u, "g", kRelAddr64}}}},
        {BSym{"g", 0, 0, 0, kClassExternal, std::nullopt}});   // g: undefined extern
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value())
        << "a kind-RESOLVED `.text` with a reloc but no atom must fail loud, not "
           "skip (Gate 2 is kind-gated, not atom-gated)";
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_CorruptedBinary));
    EXPECT_TRUE(sawDetail(rep, "reconstructed no atom"));
}

// ...and the same shape in a DATA section is now RECOVERED rather than refused
// -- D-LK-COFF-READER-ANONYMOUS-GAP-ATOMS, the row this cycle closed for data.
// The relocation lands in the synthetic atom instead of nowhere.
TEST(CoffForeignObject, KindResolvedDataSectionWithRelocButNoSymbolGapFills) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".rdata", kScnRData, std::vector<std::uint8_t>(8, 0),
              {BReloc{0u, "g", kRelAddr64}}}},
        {BSym{"g", 0, 0, 0, kClassExternal, std::nullopt}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value())
        << "an anonymous `.rdata` is gap-filled now, not refused; errs="
        << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(got->dataItems.size(), 1u);
    EXPECT_EQ(got->dataItems.front().bytes.size(), 8u)
        << "the whole anonymous section becomes ONE atom";
    ASSERT_EQ(got->dataItems.front().relocations.size(), 1u)
        << "...and the relocation that had nowhere to go now routes into it";
    EXPECT_EQ(got->dataItems.front().relocations.front().offset, 0u)
        << "at an ITEM-relative offset, which for a gap starting at 0 is 0";
    // The synthetic atom is ANONYMOUS: no ModuleSymbol may name it, or a second
    // object's identically-placed gap would fold onto it cross-CU.
    for (auto const& sy : got->symbols) {
        EXPECT_NE(sy.symbol, got->dataItems.front().symbol)
            << "a gap atom must stay module-private";
    }
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
// The vcvars64-entered cl.exe/lib.exe environment. LOCATING it is not done here —
// `native_probe::locateMsvcToolchain` is the single implementation, shared with the ABI
// conformance witnesses; this struct only USES what that returns.
//
// D-TEST-NATIVE-ORACLE-INERT-ON-POSIX — a native oracle that skips on error is a broken oracle that reports success.
//
// The lookup used to be written TWICE — once here, once inside `native_c_probe.hpp`'s
// `findCompiler` — and the copies disagreed about the same machine: this one reddened
// on a non-zero vswhere exit while that one skipped GREEN. Two implementations of one
// decision is the defect the header was hoisted to prevent, so the second one is gone
// rather than re-synchronised.
//
// NOTE the asymmetry with the ABI witnesses: the SECOND stage here was always correct.
// `env.run(...)` is consumed by `ASSERT_TRUE(...)` at every call site, so a failing
// `cl`/`lib` already went red. Only the lookup conflated, and only the lookup changed.
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
    auto const msvc = native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};
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
    ASSERT_TRUE(env.run("cl /nologo /c /GS- bar.c")) << "cl must compile bar.c";
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
    auto const msvc = native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    // `/Gy` makes `barg` a COMDAT `.text$mn` (NODUPLICATES -> Global) AND its
    // `.pdata`/`.xdata` ASSOCIATIVE(5) COMDAT sections (cl.exe-witnessed). Gate 3
    // is gated on resolved-kind, so the associative METADATA is skipped by Gate 2
    // (never fail-loud) -> a real `/Gy` object reads cleanly.
    { std::ofstream f{dir / "barg.c"};
      f << "int helper(int*p);\n"
           "int barg(int x){ int b[16]; for(int i=0;i<16;++i) b[i]=x+i; return helper(b); }\n"; }
    ASSERT_TRUE(env.run("cl /nologo /c /GS- /Gy barg.c"));
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
    auto const msvc = native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};

    { std::ofstream f{dir / "foo.c"}; f << "int foo(void){ return 42; }\n"; }
    ASSERT_TRUE(env.run("cl /nologo /c /GS- foo.c")) << "cl must compile foo.c";
    ASSERT_TRUE(std::filesystem::exists(dir / "foo.obj"));
    ASSERT_TRUE(env.run("lib /nologo /out:foo.lib foo.obj")) << "lib must wrap foo.obj";
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
    auto const msvc = native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};

    // a.c + b.c each define a DISTINCT function AND the SAME selectany COMDAT
    // datum `shared_w` (ANY selection). main references BOTH functions -> BOTH
    // members are pulled from the armap -> the two `shared_w` bodies dedup.
    { std::ofstream f{dir / "a.c"};
      f << "__declspec(selectany) int shared_w = 1;\nint alpha(void){ return 20; }\n"; }
    { std::ofstream f{dir / "b.c"};
      f << "__declspec(selectany) int shared_w = 1;\nint beta(void){ return 22; }\n"; }
    ASSERT_TRUE(env.run("cl /nologo /c /GS- a.c"));
    ASSERT_TRUE(env.run("cl /nologo /c /GS- b.c"));
    ASSERT_TRUE(env.run("lib /nologo /out:ab.lib a.obj b.obj"));
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

// ============================================================================
// D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM -- the
// END-TO-END half: a static library member that CALLS a file-local function.
//
// ★★★ WHY THE END-TO-END PIN IS NOT REDUNDANT WITH THE READER PINS.
// `tests/link/test_object_atom_coverage.cpp` proves the reader now slices a
// class-STATIC DTYPE_FUNCTION symbol into its own atom. What it CANNOT prove is
// that the chain around it agrees: that the front end still emits `static` as
// `SymbolBinding::Local`, that the writer carves the name to `sym_<id>`, that
// the archive pull hands the member to THIS reader, that the merge keeps a Local
// body instead of shadowing it, and that the resulting relocation lands on the
// right bytes. Every one of those is a place the fix could have been correct and
// useless. The defect this closes was found end-to-end, so the pin is too.
//
// ⚠ THE CONFIGURATION IS LOAD-BEARING: this must run at the BASELINE (debug)
// pipeline. At `--config=release` the inliner + DCE delete the file-local
// function outright, the call relocation never exists, and the link succeeds
// whether or not the reader was ever fixed -- the defect is INVISIBLE to any
// release-only test. `compileFiles` with no config override is the baseline, and
// that is why no override is set here.
// ============================================================================

// A two-TU COFF `.lib` built by DSS ITSELF -- no external toolchain, so this
// runs on every Windows host rather than only where MSVC is installed.
TEST(CoffLocalFunctionInArchive, DssBuiltLibMemberCallingAStaticHelperExitsFortyTwo) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "coff-local-fn"};
    auto const dir = scratch.path();

    // THE SUBJECT: `helper` is `static` -- internal linkage, invisible outside
    // this TU, and CALLED, so the member carries a relocation against a symbol
    // that only the member itself defines. That is the exact shape whose target
    // reconstructed as nothing before this fix.
    { std::ofstream f{dir / "dsslocal.c"};
      f << "static int helper(int x) { return x + 7; }\n"
           "int lib_answer(int x) { return helper(x) * 2; }\n"; }

    Program pLib;
    pLib.setOutputDir(dir);
    DiagnosticReporter libRep;
    ASSERT_EQ(pLib.compileFiles(
                  std::vector<std::string>{(dir / "dsslocal.c").string()},
                  "c-subset",
                  std::vector<std::string>{"x86_64:pe64-x86_64-windows-staticlib"},
                  libRep),
              0)
        << "the staticlib build must succeed; errs=" << libRep.errorCount();
    auto const libPath = dir / "dsslocal.lib";
    ASSERT_TRUE(std::filesystem::exists(libPath));

    // -- WHITE BOX: pull the member out of the archive and read it back. ------
    //
    // The exit code below proves the program computes the right answer; this
    // proves WHY, and it is the assertion that reds on a regression of the
    // reader specifically rather than of anything else in the chain.
    {
        std::ifstream in{libPath, std::ios::binary};
        std::vector<std::uint8_t> const libBytes{
            std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        ASSERT_FALSE(libBytes.empty());
        DiagnosticReporter arRep;
        auto arch = ffi::readArArchive(libBytes, libPath.string(), arRep);
        ASSERT_TRUE(arch.has_value()) << arch.error().detail;
        ASSERT_EQ(arch->members.size(), 1u) << "one source CU -> one member";
        auto const& mem = arch->members.front();

        auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
        ASSERT_TRUE(loaded.target && loaded.format);
        DiagnosticReporter memRep;
        auto got = pe::readRelocatableObject(
            std::span<std::uint8_t const>{libBytes}.subspan(
                static_cast<std::size_t>(mem.dataOffset),
                static_cast<std::size_t>(mem.size)),
            *loaded.target, *loaded.format, memRep, CompilationUnitId{1});
        ASSERT_TRUE(got.has_value())
            << "the archived member must read back; errs=" << memRep.errorCount();
        EXPECT_EQ(memRep.errorCount(), 0u);
        EXPECT_EQ(got->functions.size(), 2u)
            << "TWO atoms -- the exported `lib_answer` AND the file-local helper. "
               "Before the fix this was 1 and the helper's bytes belonged to "
               "nothing; the COUNT, not the byte total, is what says so";
        // The local keeps internal linkage through the round trip. Asserted by
        // counting rather than by name because DSS carves the name to a
        // synthesized `sym_<id>` whose number is an allocation detail: what must
        // hold is exactly one Local FUNCTION.
        std::size_t localFns = 0;
        for (auto const& fn : got->functions) {
            for (auto const& s : got->symbols) {
                if (s.symbol == fn.symbol && s.binding == SymbolBinding::Local) {
                    ++localFns;
                }
            }
        }
        EXPECT_EQ(localFns, 1u)
            << "the file-local function must reconstruct with Local binding -- "
               "Global would collide with another member's `sym_<n>`, and Weak "
               "would let one member's private body be silently dropped as a "
               "shadowed duplicate";
        EXPECT_NE(funcNamed(*got, "lib_answer"), nullptr)
            << "and the exported function is still itself";
    }

    // -- BLACK BOX: link main.exe against the `.lib` and RUN it. --------------
    { std::ofstream m{dir / "main.c"};
      m << "extern int lib_answer(int);\nint main(void){ return lib_answer(14); }\n"; }

    Program pMain;
    pMain.setOutputDir(dir);
    pMain.setResolveLibraries(std::vector<std::filesystem::path>{libPath});
    DiagnosticReporter rep;
    int const rc = pMain.compileFiles(
        std::vector<std::string>{(dir / "main.c").string()}, "c-subset",
        std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    ASSERT_EQ(rc, 0)
        << "linking an archive member that calls a file-local function must "
           "succeed -- this returned rc=1 with K_SymbolUndefined before the "
           "reader classified a class-STATIC DTYPE_FUNCTION symbol as an atom; "
           "errs=" << rep.errorCount();
    auto const exe = dir / "main.exe";
    ASSERT_TRUE(std::filesystem::exists(exe));

    // ★★ THE RUN IS `_WIN32`-GATED; THE BUILD ABOVE IS NOT, AND THE SPLIT IS THE
    // POINT. DSS cross-compiles a pe64 image from any host, so everything above
    // this line is a HOST-NEUTRAL assertion about the reader and belongs on every
    // leg. EXECUTING that image is a host CAPABILITY, and only Windows has it --
    // the same split `tests/program/test_static_link.cpp` already spells for its
    // pe / elf / Mach-O run arms.
    // ⚠ D-TEST-COFF-ARCHIVE-RUN-ARM-NOT-HOST-GATED: this arm shipped ungated and
    // TWO of the three legs then in use hid it. Windows runs a PE natively; WSL
    // runs one through the interop binfmt handler, so `posix_spawn` succeeds
    // there and the arm reads as portable. ✔MEASURED 2026-08-21 on the native
    // aarch64 VPS, which has neither: `posix_spawn(main.exe) failed: rc=8`
    // (ENOEXEC) -- a red that says nothing about the reader this file tests.
    // ★ The general shape: A CROSS-COMPILE TEST THAT SPAWNS ITS OUTPUT IS TWO
    // TESTS, and only the second one is about the host.
#if defined(_WIN32)
    auto const r = test_support::runBinary(exe, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE witness: (14 + 7) * 2 = 42, and the +7 exists only inside the "
           "file-local helper -- so 42 is reachable only if the helper's BYTES "
           "reached the image and the call landed on them";
#endif  // _WIN32
}

// The REAL-TOOLCHAIN variant: the same shape compiled by cl.exe and wrapped by
// lib.exe, gated exactly like the other `CoffForeignObjectNative` witnesses.
// Cheap (the pattern is this file's existing one) and worth having, because the
// discriminator this fix rests on is a claim about what OTHER producers put on
// the wire -- MEASURED as `(ty 20)(scl 3)` on cl.exe 14.51.36231 and on mingw
// gcc, and this keeps that measurement honest by running it rather than by
// asserting it in a comment.
TEST(CoffForeignObjectNative, ClObjLibMemberCallingAStaticHelperExitsFortyTwo) {
#if !defined(_WIN32)
    GTEST_SKIP() << "the native cl.exe COFF witness runs on Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-foreign"};
    auto const dir = scratch.path();
    auto const msvc = native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};

    { std::ofstream f{dir / "loc.c"};
      f << "static int helper(int x){ return x + 7; }\n"
           "int lib_answer(int x){ return helper(x) * 2; }\n"; }
    // `/Od` keeps the call: at a higher level cl would inline the helper away
    // and the object would no longer contain the shape under test -- the same
    // optimizer blindness the DSS-side pin above guards against.
    ASSERT_TRUE(env.run("cl /nologo /c /Od /GS- loc.c")) << "cl must compile loc.c";
    ASSERT_TRUE(std::filesystem::exists(dir / "loc.obj"));
    ASSERT_TRUE(env.run("lib /nologo /out:loc.lib loc.obj"));
    ASSERT_TRUE(std::filesystem::exists(dir / "loc.lib"));

    { std::ofstream m{dir / "main.c"};
      m << "extern int lib_answer(int);\nint main(void){ return lib_answer(14); }\n"; }

    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries(std::vector<std::filesystem::path>{dir / "loc.lib"});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{(dir / "main.c").string()}, "c-subset",
        std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    ASSERT_EQ(rc, 0)
        << "a real cl.exe `.lib` member calling its own `static` helper must "
           "link; errs=" << rep.errorCount();
    auto const exe = dir / "main.exe";
    ASSERT_TRUE(std::filesystem::exists(exe));

    auto const r = test_support::runBinary(exe, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "exit 42 = (14+7)*2 through a cl.exe-compiled file-local helper "
           "pulled out of a real lib.exe `.lib`";
#endif
}

// ============================================================================
// The two SIDE PROPERTIES the file-local-function classification depends on,
// each pinned on the shape a REAL producer emits.
// D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM.
// ============================================================================

// -- (i) A COMDAT section must NOT lift a file-local function's binding ------
//
// cl.exe `/Gy` puts a file-local function in its OWN COMDAT `.text$mn`
// (MEASURED on cl.exe 14.51.36231: the static helper lands in a second
// `.text$mn` carrying selection 1). So the Gate 3 lift is REACHABLE for an
// internal-linkage symbol, and skipping it there is a decision, not an omission.
//
// COMDAT Selection is a cross-object dedup policy keyed BY NAME, and internal
// linkage has no cross-object name to dedup by. Honoring the lift would be a
// miscompile in both directions: NODUPLICATES -> Global re-creates the
// `K_SymbolRedefinedAcrossUnits` collision between two members' unrelated
// `sym_<n>`, and ANY -> Weak lets one member's private body be SILENTLY dropped
// as a shadowed duplicate. RED-ON-DISABLE: consult `comdatBindingBySection` in
// the static-function arm -> this reads Weak.
TEST(CoffForeignObject, ComdatStaticFunctionKeepsInternalLinkageNotTheSelectionLift) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> const body = {0xB8, 20, 0, 0, 0, 0xC3,   // helper @ 0
                                            0xB8, 22, 0, 0, 0, 0xC3};  // pub    @ 6
    auto const obj = buildCoff(
        {BSec{".text$mn", kScnText | kScnLnkComdat, body, {}}},
        {BSym{".text$mn", 0, 1, 0, kClassStatic, kSelAny},
         BSym{"helper", 0, 1, kDtypeFunction, kClassStatic, std::nullopt},
         BSym{"pub", 6, 1, kDtypeFunction, kClassExternal, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    EXPECT_EQ(bindingOf(*got, "helper").value_or(SymbolBinding::Global),
              SymbolBinding::Local)
        << "an internal-linkage function in a COMDAT section stays Local -- the "
           "ANY selection lift applies to the EXTERNAL symbol, which is the only "
           "one another object can name";
    EXPECT_EQ(bindingOf(*got, "pub").value_or(SymbolBinding::Local),
              SymbolBinding::Weak)
        << "...and the external symbol IS lifted, so this proves the lift was "
           "live and skipped rather than absent";
    ASSERT_EQ(got->functions.size(), 2u);
}

// -- (ii) The section-definition recogniser is NAME + AUX, not AUX alone -----
//
// MEASURED on mingw gcc (Strawberry), with AND without `-g`: a file-local
// function is emitted `(sec 1)(ty 20)(scl 3)(nx 1)` -- class STATIC with ONE
// auxiliary record (COFF aux format 1, the function definition). An aux-only
// recogniser therefore calls a real producer's symbols "section identities" and
// EXEMPTS them from the atom-coverage guard, which is a silent hole in the one
// instrument that exists to make this class of loss loud.
//
// The subject below is that shape with the derived type removed, so it is a
// symbol the reader can place only by GEOMETRY: nothing on the wire says what it
// is, and its offset lies outside every atom, so the fallback in
// `object_atom_coverage.hpp` recovers it as a body. Being EXEMPTED as a section
// identity is what must never happen, because an exempted symbol is not even a
// CANDIDATE for that fallback -- its bytes reach no atom at all.
//
// ⚠ THE OBSERVABLE MOVED, AND THE ASSERTION IS STRONGER FOR IT. This pin used to
// watch for the coverage REFUSAL, which was the only visible consequence back
// when an unplaceable symbol was refused rather than recovered. A refusal is a
// boolean; what discriminates now is the SHAPE of the reconstruction, and that
// distinguishes three worlds where the boolean distinguished two: correctly
// recovered (2 atoms, 8 + 4), wrongly exempted (1 atom, 4 -- eight bytes gone),
// and wrongly absorbed (1 atom, 12 -- `mystery`'s bytes inside `entry`).
// RED-ON-DISABLE: relax `isSectionDefinitionSymbol` to the aux test alone ->
// `mystery` is never staged -> one atom of four bytes, and the eight bytes of
// `.text` ahead of `entry` belong to nothing.
TEST(CoffForeignObject, AuxBearingStaticSymbolIsNotMistakenForASectionDefinition) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> const body(12, 0xC3);
    // `.text` is NOT COMDAT here, so Gate 3 never runs and the aux record's only
    // role is to tempt the recogniser.
    auto const obj = buildCoff(
        {BSec{".text", kScnText, body, {}}},
        {BSym{"mystery", 0, 1, 0, kClassStatic, kSelAny},
         BSym{"entry", 8, 1, kDtypeFunction, kClassExternal, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value())
        << "a STATIC symbol carrying an aux record is only a SECTION IDENTITY "
           "when it is also NAMED after its section; this one is not, so it "
           "stays a candidate and geometry recovers its body; errs="
        << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(got->functions.size(), 2u)
        << "TWO atoms: an exempted `mystery` would leave one, and an absorbed "
           "one would also leave one";
    auto const* mystery = funcNamed(*got, "mystery");
    ASSERT_NE(mystery, nullptr);
    EXPECT_EQ(mystery->bytes.size(), 8u)
        << "sliced to the next boundary (`entry` at 8)";
    auto const* entry = funcNamed(*got, "entry");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->bytes.size(), 4u);
}

// -- (iii) Gate 3 reads the SECTION symbol's aux, not the first aux it finds --
//
// The same recogniser, on the other pass that uses it. gcc emits its
// aux-bearing function symbol BEFORE the section symbol of the same ordinal, so
// a lookup that takes the first aux-bearing STATIC symbol of the ordinal reads a
// FUNCTION-DEFINITION aux record and interprets byte 14 of it as a COMDAT
// Selection -- a number that means nothing. RED-ON-DISABLE: drop the name half
// of the recogniser -> `decoy`'s aux is read -> selection 5 (ASSOCIATIVE) ->
// this fails loud instead of reading green.
TEST(CoffForeignObject, ComdatSelectionComesFromTheSectionSymbolNotAPrecedingAux) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> const body = {0xB8, 42, 0, 0, 0, 0xC3};
    auto const obj = buildCoff(
        {BSec{".text$mn", kScnText | kScnLnkComdat, body, {}}},
        // `decoy` stands in for gcc's aux-bearing function symbol: same ordinal,
        // class STATIC, an aux record, and it comes FIRST. Its offset 0 is
        // covered by `pub`'s atom (equal starts), so it is not the subject --
        // the Selection lookup is.
        {BSym{"decoy", 0, 1, 0, kClassStatic, kSelAssoc},
         BSym{".text$mn", 0, 1, 0, kClassStatic, kSelNoDup},
         BSym{"pub", 0, 1, kDtypeFunction, kClassExternal, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value())
        << "the Selection must come from the symbol NAMED after the section "
           "(NODUPLICATES), not from the first aux record of the ordinal "
           "(ASSOCIATIVE, which fails loud); errs=" << rep.errorCount();
    EXPECT_FALSE(sawDetail(rep, "COMDAT Selection"));
    EXPECT_EQ(bindingOf(*got, "pub").value_or(SymbolBinding::Weak),
              SymbolBinding::Global)
        << "NODUPLICATES keeps the external symbol STRONG";
}

// ============================================================================
// THE COFF DATA HALF -- D-LK-COFF-ARCHIVE-MEMBER-READER-LOSES-STATIC-RODATA-
// SYMBOLS, the second half of D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-
// LABEL-NOT-ATOM.
//
// ★ WHY THE DERIVED TYPE CANNOT DECIDE THIS ONE. COFF stamps `notype` on EVERY
// data symbol regardless of linkage -- `pe.cpp`'s defined-DATA loop says so in
// as many words, and cl.exe and gcc agree -- so a file-local `static const`
// array is byte-for-byte the shape of an interior block label on the wire. The
// second discriminator is the SECTION's own kind: a block label is a CODE
// address, so a class-STATIC symbol in a section whose kind is not Text cannot
// be one.
//
// ⚠ THE RISK THAT RULE HAD TO SURVIVE was a foreign JUMP-TABLE label landing in
// `.rdata` and being read as a datum. ✔MEASURED on both host toolchains over a
// dense 12-case switch -- 6 cl.exe 14.51.36231 arms (/Od, /O2, /Gy, /Zi
// combinations) and 7 mingw gcc 13.2.0 arms (-O0, -O2, -g, -ffunction-sections,
// -fdata-sections, -fno-pic) -- and NEITHER producer creates that shape:
//   * cl.exe puts the jump TABLE inside `.text$mn`, at the end of its own
//     function, as a class-STATIC type-0 symbol (`$LN18` / `$LN22`), and the
//     case TARGETS in `.text` as class LABEL (6);
//   * gcc puts the table in `.rdata` but attaches NO SYMBOL to it at all,
//     reaching it from `.text` through the `.rdata` SECTION symbol plus an
//     addend -- which is why an anonymous `.rdata` region remains
//     D-LK-COFF-READER-ANONYMOUS-GAP-ATOMS and not this row.
// Both shapes are pinned below, so a future producer that does create the risky
// one reds here rather than in a linked image.
// ============================================================================

// IMAGE_SYM_CLASS_LABEL (6) -- "a label within a module", the class cl.exe
// stamps on a dense switch's CASE TARGETS. A code address by definition, which
// is why the section-kind arm is scoped to class STATIC and never to
// "non-external".
constexpr std::uint8_t kClassLabel = 6;
// IMAGE_SCN_CNT_UNINITIALIZED_DATA|MEM_READ|MEM_WRITE -- a `.bss`.
constexpr std::uint32_t kScnBss = 0xC0000080u;

// -- (a) A file-local `static const` array is its own atom -------------------
//
// The leading position, where nothing external precedes it in the section.
// RED-ON-DISABLE: drop the section-kind arm -> `table` is demoted to a bodiless
// ModuleSymbol; the geometry fallback then recovers it anyway from THIS object
// (it is uncovered), so the arm that actually proves the CLASSIFICATION rather
// than the fallback is (b).
TEST(CoffForeignObject, StaticRodataObjectIsItsOwnAtom) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    // `.rdata` = 32 bytes: a file-local `table` at [0, 16) and an exported
    // `pub` at [16, 32). Reloc-free, so the kind resolves to Rodata.
    std::vector<std::uint8_t> body(32, 0u);
    for (std::size_t i = 0; i < body.size(); ++i)
        body[i] = static_cast<std::uint8_t>(i);
    auto const obj = buildCoff(
        {BSec{".rdata", kScnRData, body, {}}},
        {BSym{"table", 0, 1, 0, kClassStatic, std::nullopt},
         BSym{"pub", 16, 1, 0, kClassExternal, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(got->dataItems.size(), 2u) << "two named objects, two atoms";
    auto const* table = dataNamed(*got, "table");
    ASSERT_NE(table, nullptr)
        << "a file-local data object must reconstruct under its own name";
    EXPECT_EQ(table->bytes.size(), 16u)
        << "sliced to the next boundary (`pub` at 16), not to the section end";
    EXPECT_EQ(table->bytes.front(), 0u);
    EXPECT_EQ(bindingOf(*got, "table").value_or(SymbolBinding::Global),
              SymbolBinding::Local)
        << "internal linkage must survive: `resolveCrossCuDefs` skips Local, "
           "which is what stops this definition satisfying another TU's extern";
    auto const* pub = dataNamed(*got, "pub");
    ASSERT_NE(pub, nullptr);
    EXPECT_EQ(pub->bytes.size(), 16u);
    EXPECT_EQ(pub->bytes.front(), 16u)
        << "...and the two atoms carry DIFFERENT bytes, so this is a real split "
           "and not two views of one slice";
}

// -- (b) THE TRAILING POSITION -- the case geometry can never see ------------
//
// ★ THIS IS THE ARM THAT PROVES THE CLASSIFICATION EXISTS. When the file-local
// object comes LAST, the exported one's atom already runs to the end of the
// section, so the local's offset IS covered: the coverage post-condition is
// silent and the geometry fallback has nothing to promote. Only a decision made
// BEFORE slicing -- class STATIC in a non-code section -- reaches it.
//
// ⚠ AND THE BYTE TOTAL CANNOT SEE IT EITHER, which is why every assertion here
// is a count or a per-atom extent. Absorbed, the module still holds all 32 bytes
// of `.rdata`; they are simply all inside `pub`.
// RED-ON-DISABLE: drop the section-kind arm -> `priv` is demoted, geometry finds
// it COVERED and leaves it, and the read returns ONE 32-byte `pub` -- green,
// with a whole named object misattributed.
TEST(CoffForeignObject, TrailingStaticRodataObjectIsNotAbsorbedIntoTheExportedOne) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> body(32, 0u);
    for (std::size_t i = 0; i < body.size(); ++i)
        body[i] = static_cast<std::uint8_t>(i);
    auto const obj = buildCoff(
        {BSec{".rdata", kScnRData, body, {}}},
        {BSym{"pub", 0, 1, 0, kClassExternal, std::nullopt},
         BSym{"priv", 16, 1, 0, kClassStatic, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    ASSERT_EQ(got->dataItems.size(), 2u)
        << "a trailing file-local object must not ride inside the exported one";
    auto const* priv = dataNamed(*got, "priv");
    ASSERT_NE(priv, nullptr);
    EXPECT_EQ(priv->bytes.size(), 16u)
        << "it takes the section's tail as its OWN atom";
    EXPECT_EQ(priv->bytes.front(), 16u);
    auto const* pub = dataNamed(*got, "pub");
    ASSERT_NE(pub, nullptr);
    EXPECT_EQ(pub->bytes.size(), 16u)
        << "...which is visible from the other side too: `pub` must STOP at the "
           "local's offset instead of swallowing the rest of the section";
}

// -- (c) `.data` and `.bss` take the same arm --------------------------------
//
// The rule is stated over the section's KIND, not over a name, so every
// non-Text kind the schema resolves must behave identically. `.bss` also
// exercises the zero-fill path, where the atom RESERVES an extent instead of
// slicing bytes -- a place a size can go wrong without any byte being wrong.
TEST(CoffForeignObject, StaticMutableAndZeroFillObjectsAreAtomsToo) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> const dataBody(8, 0xAAu);
    auto const obj = buildCoff(
        {BSec{".data", kScnData, dataBody, {}},
         BSec{".bss", kScnBss, {}, {}, /*rawSizeOverride=*/16u}},
        {BSym{"counter", 0, 1, 0, kClassStatic, std::nullopt},
         BSym{"shared", 4, 1, 0, kClassExternal, std::nullopt},
         BSym{"zeroed", 0, 2, 0, kClassStatic, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    auto const* counter = dataNamed(*got, "counter");
    ASSERT_NE(counter, nullptr) << "a file-local `.data` object is an atom";
    EXPECT_EQ(counter->bytes.size(), 4u)
        << "sliced to the next boundary (`shared` at 4)";
    EXPECT_EQ(bindingOf(*got, "counter").value_or(SymbolBinding::Global),
              SymbolBinding::Local);
    auto const* zeroed = dataNamed(*got, "zeroed");
    ASSERT_NE(zeroed, nullptr) << "a file-local `.bss` object is an atom";
    EXPECT_TRUE(zeroed->bytes.empty())
        << "a zero-fill section reserves an extent rather than slicing bytes";
    EXPECT_EQ(zeroed->reservedSize, 16u)
        << "...and the RESERVATION is the whole content of a bss atom -- a size "
           "here can be wrong while every byte is right, so `bytes.empty()` on "
           "its own asserts nothing";
}

// -- (d) THE MEASURED RISK, PINNED: cl.exe's jump table stays a label --------
//
// ✔MEASURED shape, reproduced exactly: a dense switch compiled by cl.exe leaves
// a class-STATIC, type-0 symbol (`$LN18`) at the END of its own function INSIDE
// `.text$mn`, with the case targets as class LABEL. If the section-kind arm ever
// grew to include code sections, this object would SPLIT `dense` in half at the
// jump table and every intra-function reference past that point would be
// computed against the wrong atom's base -- a silent miscompile the byte total
// cannot see, since 24 + 8 is still 32.
TEST(CoffForeignObject, ClStyleJumpTableInTextStaysInteriorToItsFunction) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> const body(32, 0x90u);
    auto const obj = buildCoff(
        {BSec{".text$mn", kScnText, body, {}}},
        {BSym{"dense", 0, 1, kDtypeFunction, kClassExternal, std::nullopt},
         BSym{"$LN4", 8, 1, 0, kClassLabel, std::nullopt},
         BSym{"$LN18", 24, 1, 0, kClassStatic, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(got->functions.size(), 1u)
        << "ONE source function, ONE atom -- neither the class-LABEL case target "
           "nor the class-STATIC jump table may become a boundary";
    EXPECT_EQ(got->functions.front().bytes.size(), 32u)
        << "`dense` must own the WHOLE section including its own jump table";
}

// -- (e) THE MEASURED gcc SHAPE: an ANONYMOUS jump table in `.rdata` ---------
//
// ✔MEASURED, mingw gcc 13.2.0 `-O2` on a dense 12-case switch: `.rdata` is 0x80
// bytes holding THREE things -- a 12-entry jump table at [0, 0x30) that NO
// SYMBOL NAMES, `msg` at 0x30, and `table` at 0x60, both class STATIC type 0 --
// and the table is reached from `.text` through a REL32 against the `.rdata`
// SECTION symbol plus an addend, with the table's own 12 entries relocating back
// into `.text`.
//
// ★★★ THIS IS THE SHAPE THAT NEEDED BOTH HALVES OF THE CYCLE. The section-kind
// arm recovers `msg` and `table`; the gap-atom pass recovers the 0x30 bytes
// between and before them. Either alone leaves the object wrong: without the
// classification the two named objects vanish, and without the gap pass a third
// of the section does. Only together does every byte of `.rdata` reach an atom.
//
// ⓘ THIS PIN USED TO ASSERT THE REFUSAL. It was written earlier in this same
// cycle, when the classification had landed and the gap pass had not, and it
// recorded the honest state at that moment: the object got further and then
// refused at relocation routing. The operator refused to ship that trade, and
// the assertion moved with the behaviour rather than the behaviour being trimmed
// to fit it.
TEST(CoffForeignObject, GccStyleAnonymousRdataJumpTableIsFullyReconstructed) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> rdata(0x80, 0u);
    for (std::size_t i = 0; i < rdata.size(); ++i)
        rdata[i] = static_cast<std::uint8_t>(i);
    std::vector<BReloc> jumpTable;
    for (std::uint32_t e = 0; e < 12u; ++e) {
        jumpTable.push_back(BReloc{e * 4u, "dense", 4u /* REL32 */});
    }
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(16, 0x90u), {}},
         BSec{".rdata", kScnRData, rdata, jumpTable}},
        {BSym{"dense", 0, 1, kDtypeFunction, kClassExternal, std::nullopt},
         BSym{"msg", 0x30, 2, 0, kClassStatic, std::nullopt},
         BSym{"table", 0x60, 2, 0, kClassStatic, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    // FOUR data atoms: the anonymous jump table [0,0x30), `msg` [0x30,0x60),
    // `table` [0x60,0x80) -- and nothing else, because 0x30+0x30+0x20 is the
    // whole section. The COUNT is asserted alongside the total: 0x80 bytes could
    // also be one atom that swallowed everything, or five that split a named
    // object at the gap boundary.
    ASSERT_EQ(got->dataItems.size(), 3u);
    std::size_t total = 0;
    for (auto const& d : got->dataItems) total += d.bytes.size();
    EXPECT_EQ(total, 0x80u)
        << "every byte of `.rdata` reaches an atom -- that is the whole claim";
    auto const* msg = dataNamed(*got, "msg");
    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->bytes.size(), 0x30u);
    EXPECT_EQ(msg->bytes.front(), 0x30u);
    auto const* table = dataNamed(*got, "table");
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->bytes.size(), 0x20u);
    EXPECT_EQ(table->bytes.front(), 0x60u);

    // The gap atom is the one no ModuleSymbol names, and it must carry the jump
    // table's OWN twelve relocations at item-relative offsets 0,4,...,0x2c.
    AssembledData const* gapAtom = nullptr;
    for (auto const& d : got->dataItems) {
        bool named = false;
        for (auto const& sy : got->symbols) named = named || (sy.symbol == d.symbol);
        if (!named) gapAtom = &d;
    }
    ASSERT_NE(gapAtom, nullptr) << "the anonymous jump table must be an atom";
    EXPECT_EQ(gapAtom->bytes.size(), 0x30u);
    EXPECT_EQ(gapAtom->bytes.front(), 0u) << "it starts at section offset 0";
    ASSERT_EQ(gapAtom->relocations.size(), 12u)
        << "all twelve jump-table entries route into the atom that owns them";
    EXPECT_EQ(gapAtom->relocations.front().offset, 0u);
    EXPECT_EQ(gapAtom->relocations.back().offset, 0x2Cu);
}

// -- (f) ...AND THE SAME SECTION WITHOUT RELOCATIONS -------------------------
//
// ⚠ THIS PIN IS THE ONE THAT CAUGHT THE REGRESSION, and it is kept in the
// stronger form rather than deleted. Mid-cycle it asserted the opposite: with
// `msg` and `table` promoted by the section-kind arm and no gap pass yet, a
// reloc-free `.rdata` read GREEN and dropped the 0x30 anonymous bytes -- a LOUD
// refusal traded for a SILENT byte loss, which this project's bar forbids even
// when the underlying row is filed elsewhere. The gap pass is what makes the
// trade unnecessary; this asserts the total is now whole.
TEST(CoffForeignObject, AnonymousRdataBytesWithNoRelocAreRecoveredNotDropped) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> rdata(0x80, 0u);
    for (std::size_t i = 0; i < rdata.size(); ++i)
        rdata[i] = static_cast<std::uint8_t>(i);
    auto const obj = buildCoff(
        {BSec{".rdata", kScnRData, rdata, {}}},
        {BSym{"msg", 0x30, 1, 0, kClassStatic, std::nullopt},
         BSym{"table", 0x60, 1, 0, kClassStatic, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(got->dataItems.size(), 3u)
        << "two named objects plus one anonymous leading gap";
    std::size_t total = 0;
    for (auto const& d : got->dataItems) total += d.bytes.size();
    EXPECT_EQ(total, 0x80u)
        << "the section is 0x80 and the reconstruction must be too -- this is "
           "the assertion that was RED before the gap pass landed";
}

// -- (g) THE BOUNDARY OF (c): a section symbol that carries NO aux record -----
//
// `isSectionDefinitionSymbol` requires BOTH halves -- the name matching the
// section AND an auxiliary record -- and that conjunction is load-bearing in the
// other direction too (gcc gives a file-local FUNCTION an aux record, so an
// aux-only test would exempt the very symbol whose loss must never be allowed;
// see `AuxBearingStaticSymbolIsNotMistakenForASectionDefinition`). The cost is
// this shape: a class-STATIC symbol NAMED after its section but with no aux is
// not recognised as an identity, so the section-kind arm treats it as a datum.
//
// ✔BOTH MEASURED TOOLCHAINS EMIT THE AUX -- cl.exe 14.51.36231 prints the
// "Section length ..." aux line under every section symbol, and mingw gcc 13.2.0
// shows `(nx 1)` + `AUX scnlen ...` on each -- and DSS's own writer emits no
// section symbols at all, so this is a shape no producer here creates. It is
// pinned because the OUTCOME changed with (c): the symbol used to be demoted and
// refused as uncovered, and now becomes an atom. That direction is the safe one
// (the bytes survive, under a name the object itself chose) and it matches what
// the geometry fallback would do with the same symbol anyway -- but it is a
// behaviour change, so it is written down rather than discovered later.
TEST(CoffForeignObject, SectionNamedStaticSymbolWithoutAuxBecomesADatum) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> body(16, 0u);
    for (std::size_t i = 0; i < body.size(); ++i)
        body[i] = static_cast<std::uint8_t>(i);
    auto const obj = buildCoff(
        {BSec{".rdata", kScnRData, body, {}}},
        {BSym{".rdata", 0, 1, 0, kClassStatic, std::nullopt},   // NO aux
         BSym{"pub", 8, 1, 0, kClassExternal, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(got->dataItems.size(), 2u);
    auto const* lead = dataNamed(*got, ".rdata");
    ASSERT_NE(lead, nullptr);
    EXPECT_EQ(lead->bytes.size(), 8u)
        << "the aux-less symbol owns [0, 8) -- no byte of the section is lost";
    EXPECT_EQ(dataNamed(*got, "pub")->bytes.size(), 8u);

    // ...and WITH the aux it is recognised as an identity again, so `pub` is the
    // section's only atom and runs the whole 16 bytes. One field apart.
    auto const withAux = buildCoff(
        {BSec{".rdata", kScnRData, body, {}}},
        {BSym{".rdata", 0, 1, 0, kClassStatic, /*auxSelection=*/kSelNoDup},
         BSym{"pub", 8, 1, 0, kClassExternal, std::nullopt}});
    DiagnosticReporter auxRep;
    auto auxGot = pe::readRelocatableObject(withAux, *loaded.target, *loaded.format,
                                            auxRep);
    ASSERT_TRUE(auxGot.has_value()) << "errs=" << auxRep.errorCount();
    // TWO items, and neither of them is the section symbol: `pub` [8,16) plus an
    // anonymous GAP atom for [0,8), which is what the section identity's own
    // bytes now become. The identity starts no atom -- the bytes ahead of `pub`
    // are recovered because NOTHING names them, not because it does.
    ASSERT_EQ(auxGot->dataItems.size(), 2u);
    auto const* auxPub = dataNamed(*auxGot, "pub");
    ASSERT_NE(auxPub, nullptr);
    EXPECT_EQ(auxPub->bytes.size(), 8u)
        << "`pub` still starts at 8 -- an identity does not move a boundary, it "
           "just is not one";
    EXPECT_EQ(dataNamed(*auxGot, ".rdata"), nullptr)
        << "and no atom is named after the section: a recognised identity is "
           "never a datum";
}

// ── EQUAL-OFFSET DEFINED SYMBOLS ARE ONE ATOM UNDER SEVERAL NAMES ──────────
//
// D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS. Two boundary symbols
// at one section offset are two NAMES for one body. Minting an atom per
// boundary produced byte-identical TWINS over one span, and `findInterval`
// hands a relocation in that span to exactly ONE of them -- the other ships
// with its call never patched. The rule, the ranking and the argument live in
// `link/format/object_atom_coverage.hpp`; this pins COFF's half of it.
//
// ⚠ THE ARRANGEMENT IS THE DISCRIMINATOR. The file-local `Lfn` is listed FIRST,
// so it holds the LOWER symbol-table index -- exactly clang's shape, where the
// section-start label precedes the external function it labels. A rule that
// broke the tie by index alone would hand the body to `Lfn`, and the exported
// name `fn` would come back as a body-less identity that no foreign linker can
// resolve. Only the externally-visible-first ranking gets this right.
//
// RED-ON-DISABLE (watched): make `resolveEqualOffsetAtomAliases` skip its
// grouping arm (`if (h - g > 1)` -> `if (false)`) -- the count goes to 3 and
// the REL32 at va 4 is routed to `Lfn`'s twin, so `fn` comes back with none.
TEST(CoffForeignObject, EqualOffsetLocalLabelSharesTheFunctionsAtomAndRelocations) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    // `.text` = 32 bytes: one body at [0, 16) named TWICE (`Lfn` and `fn`), and
    // `next` at [16, 32). Two relocations, one of each half of the rule:
    //   * va 4  -- a SITE inside the span the two names share.
    //   * va 20 -- inside `next`, TARGETING the aliased name `Lfn`.
    std::vector<std::uint8_t> body(32, 0u);
    for (std::size_t i = 0; i < body.size(); ++i)
        body[i] = static_cast<std::uint8_t>(i);
    auto const obj = buildCoff(
        {BSec{".text", kScnText, body,
              {BReloc{4u, "g", 4u /* REL32 */},
               BReloc{20u, "Lfn", 4u /* REL32 */}}}},
        {BSym{"Lfn", 0, 1, kDtypeFunction, kClassStatic, std::nullopt},
         BSym{"fn", 0, 1, kDtypeFunction, kClassExternal, std::nullopt},
         BSym{"next", 16, 1, kDtypeFunction, kClassExternal, std::nullopt},
         BSym{"g", 0, 0, 0, kClassExternal, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    // TWO atoms, pinned BY NAME. The byte total is 32 whether `Lfn` twinned
    // `fn` or not, so a sum-based assertion would pass under the exact defect
    // this test exists for.
    ASSERT_EQ(got->functions.size(), 2u)
        << "one body at [0,16) plus `next` -- `Lfn` is another NAME for the "
           "first, not a third function";
    std::vector<std::string> atomNames;
    for (auto const& f : got->functions) atomNames.push_back(nameOf(*got, f.symbol));
    std::sort(atomNames.begin(), atomNames.end());
    EXPECT_EQ(atomNames, (std::vector<std::string>{"fn", "next"}));

    auto const* fn = funcNamed(*got, "fn");
    ASSERT_NE(fn, nullptr) << "the EXTERNAL name must own the body";
    EXPECT_EQ(fn->bytes.size(), 16u) << "sliced to the next DISTINCT boundary";
    EXPECT_EQ(fn->bytes.front(), 0u);

    // The alias keeps its name and takes the body's identity -- one atom,
    // several names.
    ModuleSymbol const* alias = nullptr;
    for (auto const& s : got->symbols) if (s.name == "Lfn") { alias = &s; break; }
    ASSERT_NE(alias, nullptr) << "the aliased name must survive";
    EXPECT_EQ(alias->symbol, fn->symbol);
    EXPECT_EQ(alias->binding, SymbolBinding::Local)
        << "an alias keeps its OWN linkage -- only the body is shared";
    EXPECT_EQ(nameOf(*got, fn->symbol), "fn")
        << "an id -> row lookup keeps the FIRST row, so the canonical name must "
           "be recorded before the alias that shares its id";

    // HALF ONE -- the SITE. The REL32 at section offset 4 lies inside the span
    // the two names share and must reach the one atom, at item-relative 4.
    ASSERT_EQ(fn->relocations.size(), 1u)
        << "the call inside the shared span must be attached to the body";
    EXPECT_EQ(fn->relocations[0].offset, 4u);
    EXPECT_EQ(nameOf(*got, fn->relocations[0].target), "g");

    // HALF TWO -- the TARGET. A reloc NAMING the alias binds to the atom that
    // owns the body; an id that owns no body is `K_SymbolUndefined` at the
    // linker's compound index.
    auto const* next = funcNamed(*got, "next");
    ASSERT_NE(next, nullptr);
    ASSERT_EQ(next->relocations.size(), 1u);
    EXPECT_EQ(next->relocations[0].offset, 4u);
    EXPECT_EQ(next->relocations[0].target, fn->symbol)
        << "a target naming `Lfn` must bind to the atom `Lfn` names";
    EXPECT_EQ(nameOf(*got, next->relocations[0].target), "fn");
}

// ══ WEAK EXTERNALS — PE/COFF 5.5.3, Auxiliary Format 3 ══════════════════════
//
// D-LK-ALIAS-NAME-ABSENT-FROM-REEMITTED-OBJECT-SYMTAB, the COFF weak half.
//
// Before these, the reader's ENTIRE storage-class vocabulary was EXTERNAL(2) and
// STATIC(3); class 105 fell through an open-ended fallback and -- being UNDEF by
// construction -- was read as a plain STRONG extern, with the auxiliary record
// (the TagIndex naming the default AND the Characteristics) discarded by the
// symbol loop's aux-slot skip before anything looked at it.
//
// ⚠ THE FIXTURES BELOW ARE MEASURED SHAPES, NOT INVENTED ONES. Every field value
// comes from a raw 18-byte aux dump of a REAL mingw gcc 13.2.0 object (objdump
// renders a format-3 aux as though it were a function aux and never prints
// Characteristics at all, which is why the shape had to be read from bytes).

namespace {

// The symtab-index a reconstructed name resolves to; nullopt if the name has no
// ModuleSymbol row. Two names that share one id are two names for ONE atom.
[[nodiscard]] std::optional<SymbolId>
symIdOfName(AssembledModule const& m, std::string const& name) {
    for (auto const& s : m.symbols) if (s.name == name) return s.symbol;
    return std::nullopt;
}
[[nodiscard]] bool hasExternNamed(AssembledModule const& m, std::string const& n) {
    for (auto const& e : m.externImports) if (e.mangledName == n) return true;
    return false;
}

constexpr std::uint8_t  kClassWeakExternal  = 105u;
constexpr std::uint32_t kWeakSearchNoLibrary = 1u;
constexpr std::uint32_t kWeakSearchLibrary   = 2u;
constexpr std::uint32_t kWeakSearchAlias     = 3u;

}  // namespace

// -- (a) A SECTION-BACKED default: the weak name IS that body ----------------
//
// The shape mingw gcc emits for a weak function DEFINITION: the body is renamed
// to a `.weak.`-prefixed symbol and kept EXTERNAL/STRONG, and the real name
// becomes an UNDEF class-105 record whose aux TagIndex names the renamed body.
// The reconstruction must put the body back under its REAL name, WEAK.
TEST(CoffWeakExternal, SectionBackedDefaultBindsTheWeakNameToThatBody) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> text(0x20, 0x90u);
    auto const obj = buildCoff(
        {BSec{".text", kScnText, text, {}}},
        {// the renamed strong body at `.text`+0 ...
         BSym{"Wbody", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         // ... and `caller` after it, so the body has a real extent
         BSym{"caller", 0x10, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         // the weak external deferring to it
         BSym{"wfn", 0, 0, kDtypeFunction, kClassWeakExternal, std::nullopt,
              BWeakAux{"Wbody", kWeakSearchNoLibrary, std::nullopt}}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    // THE CLAIM, and it is the defect this arm removes: the weak name is DEFINED
    // here. It used to come back as an extern import, so the linker reported
    // "undefined symbol" about a symbol defined in the object it had just read.
    EXPECT_FALSE(hasExternNamed(*got, "wfn"))
        << "a weak external whose default is defined HERE is not an import";
    auto const* body = funcNamed(*got, "wfn");
    ASSERT_NE(body, nullptr) << "the body must be reachable under its REAL name";
    EXPECT_EQ(body->bytes.size(), 0x10u)
        << "and it must be the BODY, sliced to the next boundary -- not an "
           "empty row that merely carries the name";

    // The binding is the record's own statement: a weak external is the name
    // that YIELDS. Asserting it is the point -- a Global here would make an
    // overridable name collide with the definition meant to override it.
    EXPECT_EQ(bindingOf(*got, "wfn"), SymbolBinding::Weak);
    EXPECT_EQ(bindingOf(*got, "Wbody"), SymbolBinding::Global)
        << "the renamed body keeps the STRONG linkage the object gave it";

    // One atom, two names -- the equal-offset alias rule, not two twin atoms.
    auto const wfnId  = symIdOfName(*got, "wfn");
    auto const bodyId = symIdOfName(*got, "Wbody");
    ASSERT_TRUE(wfnId.has_value() && bodyId.has_value());
    EXPECT_EQ(*wfnId, *bodyId)
        << "the weak name and the default name address ONE body";
    EXPECT_EQ(got->functions.size(), 2u)
        << "two functions -- the shared body and `caller` -- never three";
}

// -- (b) A NON-section-backed default: fail loud, and name the missing fact ---
//
// gcc's encoding of a weak UNDEFINED reference: the default is an ABSOLUTE
// symbol of value 0, so an unresolved reference tests as 0. DSS's link tier
// cannot carry that -- ExternImport declares no binding on ANY format -- so the
// reader REFUSES rather than silently promoting the name to a strong extern.
TEST(CoffWeakExternal, AbsoluteDefaultIsAWeakUndefinedReferenceAndFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x10, 0x90u), {}}},
        {BSym{"probe", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         // the ABSOLUTE(-1) zero fallback gcc emits
         BSym{"Wabs", 0, 0xFFFFu, 0, kClassExternal, std::nullopt, std::nullopt},
         BSym{"maybe", 0, 0, kDtypeFunction, kClassWeakExternal, std::nullopt,
              BWeakAux{"Wabs", kWeakSearchNoLibrary, std::nullopt}}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_CorruptedBinary));
    EXPECT_TRUE(sawDetail(rep, "WEAK UNDEFINED REFERENCE"))
        << "the diagnostic must name the construct, not just refuse";
    EXPECT_TRUE(sawDetail(rep, "D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE"))
        << "and it must point at the row that owns the missing carrier";
}

// -- (c) THE PIN THAT KEEPS THE DISPATCH TOTAL -------------------------------
//
// An unmodeled storage class must FAIL LOUD, not land in the block-label
// bucket. Without this, the next class someone's toolchain emits is silently
// reclassified -- which is exactly how class 105 became a strong extern.
// IMAGE_SYM_CLASS_FAR_EXTERNAL (0x44) is a real spec class this reader does not
// model, so the fixture asks a genuine question rather than a made-up one.
TEST(CoffWeakExternal, UnmodeledStorageClassFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x10, 0x90u), {}}},
        {BSym{"fn", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"odd", 8, 1, 0, 0x44u /* FAR_EXTERNAL */, std::nullopt, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_CorruptedBinary));
    EXPECT_TRUE(sawDetail(rep, "storage class 68"));
    EXPECT_TRUE(sawDetail(rep, "does not model"));
}

// A class the dispatch DOES model as bodiless keeps reading green -- the
// enumeration must not have turned a working shape into a refusal. LABEL(6) is
// what cl.exe stamps on its switch-case targets.
TEST(CoffWeakExternal, ModeledBodilessStorageClassStillReadsGreen) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x10, 0x90u), {}}},
        {BSym{"fn", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"LN4", 8, 1, 0, 6u /* IMAGE_SYM_CLASS_LABEL */, std::nullopt, std::nullopt}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    EXPECT_EQ(got->functions.size(), 1u)
        << "an interior LABEL stays interior -- it must not split the function";
}

// -- (d) An unmodeled Characteristics value fails loud -----------------------
//
// ANTI_DEPENDENCY(4) means sym1 must NOT force sym2 to be pulled from an
// archive. Reading it as a plain weak external would change which members a
// static link pulls, so it is refused rather than defaulted.
TEST(CoffWeakExternal, UnmodeledWeakExternCharacteristicsFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x20, 0x90u), {}}},
        {BSym{"Wbody", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"caller", 0x10, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"wfn", 0, 0, kDtypeFunction, kClassWeakExternal, std::nullopt,
              BWeakAux{"Wbody", 4u /* ANTI_DEPENDENCY */, std::nullopt}}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawDetail(rep, "Characteristics 4"));
    EXPECT_TRUE(sawDetail(rep, "ANTI_DEPENDENCY"));
}

// SEARCH_LIBRARY(2) and SEARCH_ALIAS(3) are modeled and reconstruct identically
// to NOLIBRARY(1) -- they differ only in how hard a LINKER should look for sym1,
// which is not a question DSS's object tier asks. Asserted so the vocabulary's
// ACCEPTED half is pinned too, not only its refusal.
TEST(CoffWeakExternal, EverySearchCharacteristicReconstructsTheSameRelation) {
    for (std::uint32_t chars : {kWeakSearchNoLibrary, kWeakSearchLibrary,
                                kWeakSearchAlias}) {
        auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
        ASSERT_TRUE(loaded.target && loaded.format);
        auto const obj = buildCoff(
            {BSec{".text", kScnText, std::vector<std::uint8_t>(0x20, 0x90u), {}}},
            {BSym{"Wbody", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
             BSym{"caller", 0x10, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
             BSym{"wfn", 0, 0, kDtypeFunction, kClassWeakExternal, std::nullopt,
                  BWeakAux{"Wbody", chars, std::nullopt}}});
        DiagnosticReporter rep;
        auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
        ASSERT_TRUE(got.has_value()) << "characteristics=" << chars
                                     << " errs=" << rep.errorCount();
        EXPECT_EQ(bindingOf(*got, "wfn"), SymbolBinding::Weak)
            << "characteristics=" << chars;
        EXPECT_EQ(symIdOfName(*got, "wfn"), symIdOfName(*got, "Wbody"))
            << "characteristics=" << chars;
    }
}

// -- (e) Structural refusals on the record's own shape -----------------------

TEST(CoffWeakExternal, WeakExternalWithNoAuxiliaryRecordFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x10, 0x90u), {}}},
        {BSym{"fn", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"wfn", 0, 0, kDtypeFunction, kClassWeakExternal, std::nullopt,
              std::nullopt}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawDetail(rep, "carries no auxiliary record"));
}

TEST(CoffWeakExternal, WeakExternalTagIndexPastTheTableFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x10, 0x90u), {}}},
        {BSym{"fn", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"wfn", 0, 0, kDtypeFunction, kClassWeakExternal, std::nullopt,
              BWeakAux{"", kWeakSearchNoLibrary, std::uint32_t{9999u}}}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawDetail(rep, "names default symbol index 9999"));
}

TEST(CoffWeakExternal, WeakExternalThatIsNotUndefFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    // 5.5.3 states UNDEF + value zero as part of the record's identity; a
    // section-backed class-105 record is not a weak external, and decoding its
    // aux as format 3 would read arbitrary bytes as a symbol index.
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x20, 0x90u), {}}},
        {BSym{"Wbody", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"wfn", 0x10, 1, kDtypeFunction, kClassWeakExternal, std::nullopt,
              BWeakAux{"Wbody", kWeakSearchNoLibrary, std::nullopt}}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawDetail(rep, "requires UNDEF"));
}

// PE/COFF 5.5.3 states the record's identity as a CONJUNCTION -- SectionNumber
// UNDEF **and** Value zero -- and the reader spends one refusal arm on each.
// The arm above is reached by a fixture carrying sectNum 1, which means the
// UNDEF arm fires FIRST and its assertion ("requires UNDEF") is the only thing
// that test has ever read. The Value arm had NO input that reached it: a
// refusal nothing reaches is a refusal nobody has run, and it would have gone
// on reading as covered forever. This cell supplies UNDEF **with** a non-zero
// Value, so the second conjunct is the only one that can fire -- and asserts
// the FIRST arm's sentence is ABSENT, because a fixture that trips both proves
// nothing about which one is live.
TEST(CoffWeakExternal, WeakExternalWithNonZeroValueFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x20, 0x90u), {}}},
        {BSym{"Wbody", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"wfn", 0x10 /* Value != 0 */, 0 /* UNDEF, so the first arm passes */,
              kDtypeFunction, kClassWeakExternal, std::nullopt,
              BWeakAux{"Wbody", kWeakSearchNoLibrary, std::nullopt}}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawDetail(rep, "requires zero"))
        << "a class-105 record with a non-zero Value is not a weak external, "
           "and reading its aux as Auxiliary Format 3 would decode arbitrary "
           "bytes as a symbol index";
    EXPECT_FALSE(sawDetail(rep, "requires UNDEF"))
        << "this fixture must reach the VALUE arm and only it -- if the "
           "SectionNumber arm also fired, the cell says nothing about which "
           "refusal is live";
}

// The `auxSlot[tagIndex]` arm, which is NOT the past-the-table arm above it.
// The only fixture that named a raw TagIndex used `9999u`, which is past the
// table -- so the earlier bound check fired and this arm, the one that catches a
// TagIndex landing INSIDE the table but on a slot that is not a symbol, had
// never executed. The distinction is the whole reason both exist: `syms[tag]`
// for an aux slot reads a default-constructed `Sym` (sectNum 0, empty name) and
// would route the weak external down the UNDEFINED-default path, silently
// turning a name that defers into a name that imports.
//
// The shape: an aux-BEARING static symbol occupies slot 0, so slot 1 is an
// auxiliary record; the weak external names slot 1 as its default. `.text` is
// deliberately NOT COMDAT, so Gate 3 never runs and the aux exists only to make
// slot 1 an aux slot.
TEST(CoffWeakExternal, WeakExternalTagIndexNamingAnAuxSlotFailsLoud) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x20, 0x90u), {}}},
        {BSym{"mystery", 0, 1, 0, kClassStatic, kSelAny, std::nullopt},
         BSym{"Wbody", 0x10, 1, kDtypeFunction, kClassExternal, std::nullopt,
              std::nullopt},
         BSym{"wfn", 0, 0, kDtypeFunction, kClassWeakExternal, std::nullopt,
              BWeakAux{"", kWeakSearchNoLibrary, std::uint32_t{1u}}}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawDetail(rep, "AUXILIARY slot"))
        << "TagIndex 1 is inside the table but names `mystery`'s auxiliary "
           "record, not a symbol";
    EXPECT_FALSE(sawDetail(rep, "past the symbol table"))
        << "this cell must reach the aux-slot arm, not the bound arm the "
           "existing 9999u fixture already covers";
}

// A NAMELESS weak external. This arm used to be `if (s.name.empty()) continue;`
// -- a silent DROP -- and nothing exercised it either way, so the behaviour was
// whatever the line happened to say. It is now a refusal, and the reason is the
// index: the record still occupies a slot in NumberOfSymbols, so a relocation
// can name it, and a dropped record leaves that relocation retargeted to a
// SymbolId the reader never produced. That surfaces as an unresolved symbol
// against whoever merged the object, arbitrarily far from the malformed record
// that caused it. Every other structural violation of 5.5.3 in this decoder
// fails loud; this one now does too.
TEST(CoffWeakExternal, NamelessWeakExternalFailsLoudRatherThanBeingDropped) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x10, 0x90u), {}}},
        {BSym{"Wbody", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"", 0, 0, kDtypeFunction, kClassWeakExternal, std::nullopt,
              BWeakAux{"Wbody", kWeakSearchNoLibrary, std::nullopt}}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value())
        << "a nameless WEAK_EXTERNAL must be refused, not skipped";
    EXPECT_TRUE(sawDetail(rep, "EMPTY name"));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_CorruptedBinary));
}

// -- (f) COMMON symbols: a DEFINITION, never an import -----------------------
//
// Found while making the storage-class dispatch total, and it is a silent wrong
// answer in the worst direction. PE/COFF 5.4.2: an EXTERNAL record with UNDEF
// section number and a NON-ZERO Value is a COMMON symbol whose Value is its
// SIZE. The reader read it as an extern import -- so an object that DEFINES
// storage was reconstructed as one that DEMANDS it, and the definition vanished
// on re-emission. ✔MEASURED, mingw gcc 13.2.0 with -fcommon: a tentative
// definition emits (sec 0)(ty 0)(scl 2) with Value 4.
TEST(CoffWeakExternal, CommonSymbolFailsLoudRatherThanReadingAsAnImport) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x10, 0x90u), {}}},
        {BSym{"fn", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"cvar", 4 /* SIZE, not an offset */, 0, 0, kClassExternal,
              std::nullopt, std::nullopt}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawDetail(rep, "COMMON symbol"));
    // A zero-Value UNDEF external is an ordinary import and must stay one --
    // the guard has to discriminate, not merely refuse UNDEF externals.
    auto const ok = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x10, 0x90u), {}}},
        {BSym{"fn", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"imp", 0, 0, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt}});
    DiagnosticReporter rep2;
    auto got2 = pe::readRelocatableObject(ok, *loaded.target, *loaded.format, rep2);
    ASSERT_TRUE(got2.has_value()) << "errs=" << rep2.errorCount();
    EXPECT_TRUE(hasExternNamed(*got2, "imp"));
}

// ══ THE SAME CLAIM AGAINST THE REAL PRODUCER ════════════════════════════════
//
// The fixtures above are hand-built from a MEASURED shape. These compile the
// shape with the actual toolchain, so the reader is asked about bytes nobody in
// this repo typed -- which name the aux TagIndex really points at, what
// Characteristics gcc really writes, and what it really does to the body's name.
// A transcription error in the fixtures above survives them and dies here.

namespace {

#if defined(_WIN32)
// A mingw gcc on PATH, or nothing. LOCATE, then PROVE IT BUILDS -- the same
// skip-vs-fail discipline as `native_probe::locateMsvcToolchain`
// (D-TEST-NATIVE-ORACLE-INERT-ON-POSIX): a tool that is ABSENT is a skip, a tool
// that is PRESENT and fails is a RED. Gating on `where gcc` alone would call a
// broken install "absent" and quietly retire the witness.
struct MingwGcc {
    bool        usable = false;
    std::string detail;
    std::filesystem::path work;

    // Compile `src` to `obj` with gcc. Returns false on any non-zero exit.
    [[nodiscard]] bool compile(std::string const& source,
                               std::string const& stem,
                               std::filesystem::path& objOut) const {
        auto const src = work / (stem + ".c");
        { std::ofstream s{src}; s << source; }
        objOut = work / (stem + ".o");
        std::error_code ec;
        std::filesystem::remove(objOut, ec);   // a stale .o must not vouch
        std::string const cmd = "gcc -c -O0 -o \"" + objOut.string() + "\" \""
                              + src.string() + "\" >nul 2>&1";
        return std::system(cmd.c_str()) == 0
            && std::filesystem::exists(objOut);
    }
};

[[nodiscard]] MingwGcc locateMingwGcc(std::filesystem::path const& work) {
    MingwGcc g;
    g.work = work;
    if (std::system("where gcc >nul 2>&1") != 0) {
        g.detail = "no gcc on PATH -- the mingw witness is inert on this host";
        return g;
    }
    // PRESENT: prove it can build before promising a red on failure.
    std::filesystem::path probe;
    MingwGcc probeGcc; probeGcc.work = work;
    if (!probeGcc.compile("int p(void){return 0;}\n", "gcc_toolchain_check", probe)) {
        g.detail = "gcc is on PATH but could not compile "
                   "`int p(void){return 0;}` -- a shim with no toolchain behind "
                   "it counts as ABSENT, not broken";
        return g;
    }
    g.usable = true;
    return g;
}
#endif  // _WIN32

}  // namespace

// A REAL mingw weak DEFINITION reads back with the body under its REAL name and
// binding Weak -- not as "undefined symbol", which is what this reader produced
// for every gcc weak definition before the WEAK_EXTERNAL arm existed.
TEST(CoffWeakExternalNative, RealMingwWeakDefinitionBindsTheBodyToItsRealName) {
#if !defined(_WIN32)
    GTEST_SKIP() << "compiles a mingw-gcc COFF object; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-weak"};
    auto const gcc = locateMingwGcc(scratch.path());
    if (!gcc.usable) GTEST_SKIP() << gcc.detail;

    std::filesystem::path obj;
    ASSERT_TRUE(gcc.compile(
        "__attribute__((weak)) int wfn(int x) { return x + 1; }\n"
        "int caller(int x) { return wfn(x); }\n",
        "weakdef", obj)) << "gcc is usable, so a failure here is a RED";

    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const bytes = readFile(obj);
    ASSERT_FALSE(bytes.empty());

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(bytes, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value())
        << "a gcc weak definition must READ; errs=" << rep.errorCount();

    // `wfn` is DEFINED by this object. Assert the resolved NAME and BINDING --
    // "the read returned success" would have been satisfied by the old
    // behaviour too, which read `wfn` as an undefined strong extern.
    EXPECT_FALSE(hasExternNamed(*got, "wfn"))
        << "`wfn` is defined in the very object being read";
    auto const* body = funcNamed(*got, "wfn");
    ASSERT_NE(body, nullptr) << "the body must carry its real name";
    EXPECT_GT(body->bytes.size(), 0u);
    EXPECT_EQ(bindingOf(*got, "wfn"), SymbolBinding::Weak)
        << "a weak definition that reads back Global is one that can no longer "
           "be overridden -- the whole content of the attribute";

    // gcc's renamed body symbol is `.weak.<name>.<referrer>`; the reader must
    // land BOTH names on ONE atom. Found by prefix, because the suffix is
    // gcc's choice of referring function, not a stable contract.
    ModuleSymbol const* renamed = nullptr;
    for (auto const& s : got->symbols) {
        if (s.name.rfind(".weak.wfn.", 0) == 0) { renamed = &s; break; }
    }
    ASSERT_NE(renamed, nullptr)
        << "gcc renames the weak body; if this vanished, the shape changed";
    EXPECT_EQ(renamed->symbol, *symIdOfName(*got, "wfn"))
        << "the renamed body and the real name address ONE atom";
    EXPECT_EQ(renamed->binding, SymbolBinding::Global);

    // And `caller`'s call must reach that atom rather than a dangling id.
    auto const* caller = funcNamed(*got, "caller");
    ASSERT_NE(caller, nullptr);
    ASSERT_EQ(caller->relocations.size(), 1u);
    EXPECT_EQ(caller->relocations[0].target, *symIdOfName(*got, "wfn"))
        << "the call to `wfn` binds to the atom `wfn` names";
#endif
}

// A REAL mingw `__attribute__((weak, alias("target")))` -- the shape the WRITER
// half emits -- reads back as the alias name on the target's body, weak.
TEST(CoffWeakExternalNative, RealMingwWeakAliasBindsBothNamesToOneBody) {
#if !defined(_WIN32)
    GTEST_SKIP() << "compiles a mingw-gcc COFF object; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-weak"};
    auto const gcc = locateMingwGcc(scratch.path());
    if (!gcc.usable) GTEST_SKIP() << gcc.detail;

    std::filesystem::path obj;
    ASSERT_TRUE(gcc.compile(
        "int real_fn(int x) { return x * 2; }\n"
        "__attribute__((weak, alias(\"real_fn\"))) int alias_fn(int x);\n"
        "int use(int x) { return alias_fn(x); }\n",
        "weakalias", obj)) << "gcc is usable, so a failure here is a RED";

    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(readFile(obj), *loaded.target,
                                         *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();

    EXPECT_FALSE(hasExternNamed(*got, "alias_fn"));
    EXPECT_EQ(bindingOf(*got, "alias_fn"), SymbolBinding::Weak);
    EXPECT_EQ(bindingOf(*got, "real_fn"), SymbolBinding::Global);
    auto const aliasId = symIdOfName(*got, "alias_fn");
    auto const realId  = symIdOfName(*got, "real_fn");
    ASSERT_TRUE(aliasId.has_value() && realId.has_value());
    EXPECT_EQ(*aliasId, *realId)
        << "an alias and its target are two names for ONE body";
#endif
}

// A REAL mingw weak UNDEFINED REFERENCE -- the half DSS's symbol model cannot
// yet carry. It must be REFUSED with a diagnostic naming the missing fact, not
// read as a strong extern. ⚠ THIS TEST RECORDS A GAP, NOT A CAPABILITY: gcc and
// clang both LINK this construct and let the reference test as zero. When the
// carrier lands, this pin flips to asserting the link succeeds.
TEST(CoffWeakExternalNative, RealMingwWeakUndefinedReferenceIsRefusedNotDowngraded) {
#if !defined(_WIN32)
    GTEST_SKIP() << "compiles a mingw-gcc COFF object; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-weak"};
    auto const gcc = locateMingwGcc(scratch.path());
    if (!gcc.usable) GTEST_SKIP() << gcc.detail;

    std::filesystem::path obj;
    ASSERT_TRUE(gcc.compile(
        "extern __attribute__((weak)) int maybe(void);\n"
        "int probe(void) { return maybe ? maybe() : 42; }\n",
        "weakundef", obj)) << "gcc is usable, so a failure here is a RED";

    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(readFile(obj), *loaded.target,
                                         *loaded.format, rep);
    EXPECT_FALSE(got.has_value())
        << "reading it green would mean the weak flag was silently dropped";
    EXPECT_TRUE(sawDetail(rep, "WEAK UNDEFINED REFERENCE"));
    EXPECT_TRUE(sawDetail(rep, "D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE"));
#endif
}

// ★★★ THE END-TO-END WITNESS, AND IT EXERCISES THE EXACT CONSUMER THE OPERATOR
// RULING NAMES: RE-EMISSION OF AN OBJECT DSS READ.
//
// [[D-LK-PE-ALTERNATENAME-DECLARE-AND-REFUSE]] declined to build this writer on
// 2026-08-05, on a measured premise: zero consumers, plus a named front-end
// blocker ([[D-CSUBSET-ATTRIBUTE-ALIAS-TARGET-NO-SURFACE]] -- the c-subset
// cannot express `__attribute__((alias(...)))`, so a writer would be a path
// nothing could invoke). That premise CHANGED, and this test is the proof: the
// consumer is not a front end at all. gcc writes the alias, DSS READS it, DSS
// RE-EMITS it, and a FOREIGN LINKER links the re-emitted object and RUNS it.
//
// Structural inspection is deliberately NOT this test. A foreign linker
// resolving both names and a binary returning 42 is a claim no byte pin makes:
// a symbol table can be perfectly shaped and still name the wrong index.
//
// ⚠⚠ WHY link.exe AND NOT gcc's OWN LINKER -- do NOT "fix" this to use gcc.
// ✔MEASURED, with gcc's own object as the CONTROL, which is what makes the
// attribution sound (a control that does not match the target is how two cycles
// were lost once before):
//   * mingw GNU ld 13.2.0 REFUSES a cross-object weak alias at every
//     Characteristics value -- and refuses GCC'S OWN UNMODIFIED OBJECT
//     identically, in both link orders and from an archive
//     (`undefined reference to 'alias_fn'`). Its PE backend does not export a
//     weak-external name across objects.
//   * MSVC link.exe resolves it and the binary runs, for a DSS-written object
//     and for a gcc-written one alike.
// ⇒ the ld refusal is a NON-DSS CONFOUND, not a DSS defect: bar §A.3b makes the
// test the DISJUNCTION, and link.exe is a reference that works. gcc's own
// `weak, alias(...)` links only when the reference sits in the SAME translation
// unit, where gcc has already bound it to the renamed `.weak.<n>.<n>` body and
// the weak external is never consulted at all.
TEST(CoffWeakExternalNative, ForeignLinkerConsumesADssReEmittedWeakAlias) {
#if !defined(_WIN32)
    GTEST_SKIP() << "links with MSVC link.exe; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-weak"};
    auto const dir = scratch.path();
    auto const gcc = locateMingwGcc(dir);
    if (!gcc.usable) GTEST_SKIP() << gcc.detail;
    auto const msvc = native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};

    // 1. gcc writes a weak alias of a strong definition.
    std::filesystem::path src;
    ASSERT_TRUE(gcc.compile(
        "int real_fn(int x) { return x * 2; }\n"
        "__attribute__((weak, alias(\"real_fn\"))) int alias_fn(int x);\n",
        "aliassrc", src)) << "gcc is usable, so a failure here is a RED";

    // 2. DSS reads it.
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    DiagnosticReporter rrep;
    auto mod = pe::readRelocatableObject(readFile(src), *loaded.target,
                                         *loaded.format, rrep);
    ASSERT_TRUE(mod.has_value()) << "errs=" << rrep.errorCount();

    // 3. DSS re-emits it. This is the arm that used to FAIL LOUD: the alias
    //    binds Weak where its canonical binds Global.
    DiagnosticReporter wrep;
    auto const reemitted = pe::encode(*mod, *loaded.target, *loaded.format, wrep);
    ASSERT_FALSE(reemitted.empty())
        << "re-emitting a weak alias must now succeed; errs=" << wrep.errorCount();
    EXPECT_EQ(wrep.errorCount(), 0u);
    auto const objPath = dir / "dss_reemitted.obj";
    {
        std::ofstream o{objPath, std::ios::binary};
        o.write(reinterpret_cast<char const*>(reemitted.data()),
                static_cast<std::streamsize>(reemitted.size()));
    }

    // 4. A FOREIGN LINKER resolves BOTH names out of the DSS-written object.
    //    `main` lives in a DIFFERENT translation unit, which is the whole
    //    point: an alias only reachable from inside its own object would be
    //    satisfied by gcc's internal renaming and would prove nothing about the
    //    weak-external record.
    auto const mainSrc = dir / "wmain.c";
    {
        std::ofstream m{mainSrc};
        m << "int real_fn(int);\n"
             "int alias_fn(int);\n"
             "int main(void) { return real_fn(10) + alias_fn(11); }\n";
    }
    ASSERT_TRUE(env.run("cl /c /nologo wmain.c /Fowmain.obj"))
        << "the reference C compiler must build the referencing TU";
    ASSERT_TRUE(env.run("link /nologo /OUT:wmain.exe wmain.obj dss_reemitted.obj"))
        << "link.exe must resolve BOTH `real_fn` and `alias_fn` out of the "
           "DSS-written object -- a weak external whose aux TagIndex named the "
           "wrong record, or whose Characteristics is not SEARCH_ALIAS, fails "
           "here with LNK2019";
    auto const exe = dir / "wmain.exe";
    ASSERT_TRUE(std::filesystem::exists(exe));

    // 5. ...and it RUNS. 10*2 + 11*2 = 42, which is only reachable if BOTH
    //    names reached the SAME body.
    auto const r = test_support::runBinary(exe, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE witness: a gcc weak alias, read by DSS, re-emitted by DSS as an "
           "IMAGE_SYM_CLASS_WEAK_EXTERNAL + Auxiliary Format 3 record, linked by "
           "a foreign linker and executed";
    // ⓘ WHAT THIS WITNESS DOES **NOT** CATCH, stated because an earlier draft of
    // this comment claimed it did and a mutant proved otherwise. Advancing the
    // aux TagIndex by one still links and still exits 42: in this object the
    // next symbol is `.weak.alias_fn.real_fn`, gcc's renamed body, which sits at
    // the SAME address. An execution witness cannot separate two names for one
    // address -- that is what `PeWriter.WeakAliasOfAStrongDefinitionEmitsA
    // WeakExternalRecord` (TagIndex asserted by value) and
    // `PeWriter.WeakAliasRoundTripsThroughDssOwnReader` are for, and both DO red
    // on that mutant. Keep all three: this one proves a foreign toolchain
    // accepts the record at all, which no byte pin can.
#endif
}

// A weak DATA definition takes the same arm -- the weak-external route stages
// its name in whatever section the default lives in, with no code/data gate.
// Pinned separately because "no gate" is a property that a future edit can
// remove without any function-shaped test noticing: `.data` and `.text` reach
// the slicing loop through different predicates everywhere else in this reader.
TEST(CoffWeakExternalNative, RealMingwWeakDataDefinitionBindsToItsRealName) {
#if !defined(_WIN32)
    GTEST_SKIP() << "compiles a mingw-gcc COFF object; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-weak"};
    auto const gcc = locateMingwGcc(scratch.path());
    if (!gcc.usable) GTEST_SKIP() << gcc.detail;

    std::filesystem::path obj;
    ASSERT_TRUE(gcc.compile(
        "__attribute__((weak)) int wdata = 7;\n"
        "int *get(void) { return &wdata; }\n",
        "weakdata", obj)) << "gcc is usable, so a failure here is a RED";

    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(readFile(obj), *loaded.target,
                                         *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();

    EXPECT_FALSE(hasExternNamed(*got, "wdata"))
        << "the object DEFINES `wdata`; an import row here is the same defect "
           "the function case had";
    EXPECT_EQ(bindingOf(*got, "wdata"), SymbolBinding::Weak);
    auto const* d = dataNamed(*got, "wdata");
    ASSERT_NE(d, nullptr) << "the datum must be reachable under its real name";
    // The initialiser is the strongest available property: a row that carries
    // the name but not the bytes would satisfy every assertion above.
    ASSERT_GE(d->bytes.size(), 4u);
    EXPECT_EQ(d->bytes[0], 7u) << "the weak datum's initialiser, little-endian";
#endif
}

// ── (h) A NAMELESS UNDEF RECORD: REFUSED, NOT DROPPED ────────────────────────
//    D-LK-COFF-NAMELESS-UNDEF-EXTERN-SILENTLY-DROPPED.
//
// The twin of `NamelessWeakExternalFailsLoudRatherThanBeingDropped` above, and
// it was left alone when that one landed BECAUSE it is a decision rather than a
// fix: unlike the weak external it carries no aux relation, and its skip was
// DOCUMENTED as deliberate ("a nameless slot carries no import identity").
//
// ★★ THE DECISION IS REFUSAL, AND THE MEASUREMENT THAT SETTLES IT IS THE INDEX.
// The record still occupies a `NumberOfSymbols` slot, so a relocation can name
// it BY INDEX, and every gate that would catch such a relocation lets it
// through: the bound check passes (the index is real), the aux-slot check
// passes (it is not an aux), and `rel.target = SymbolId{ownerOf(symIdx)}` then
// names a SymbolId this reader never produced. ✔MEASURED by reading the
// relocation loop's own conclusion at (6.44) -- "an id that owns no body is
// `K_SymbolUndefined` at the linker's compound index". So the drop does not
// vanish; it re-emerges at MERGE time as an unresolved symbol with NO NAME TO
// PRINT, attributed to whoever merged the object rather than to the malformed
// record that caused it.
//
// ★ AND THE RECORD IS MALFORMED, NOT UNMODELED. PE/COFF 5.4.2 makes an UNDEF
// record with Value 0 "a reference to an external symbol defined elsewhere" --
// a reference resolved BY NAME. A nameless one names nothing any object could
// satisfy, so there is no meaning DSS is declining to model; there is only a
// record that cannot be represented, which the bar says must refuse by name.
//
// ⚠ BLAST RADIUS MEASURED BEFORE CHANGING IT, because it lands on objects DSS
// did not write: the `CoffForeignObjectNative` witnesses over real cl.exe /
// clang-cl objects (a `/GS-` object, a `/Gy` object, a multi-member `.lib`, and
// the mingw-gcc probes) stay green, i.e. no real producer emits this shape.
TEST(CoffObjectReader, NamelessUndefExternFailsLoudRatherThanBeingDropped) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x10, 0x90u), {}}},
        {BSym{"fn", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         // UNDEF(0), EXTERNAL, Value 0 -- an import -- with NO NAME.
         BSym{"", 0, 0, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value())
        << "a nameless UNDEF record must be refused, not skipped -- its slot is "
           "still addressable by a relocation";
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_CorruptedBinary));
    EXPECT_TRUE(sawDetail(rep, "EMPTY name"));
    EXPECT_TRUE(sawDetail(rep, "BY INDEX"))
        << "the diagnostic must name the HAZARD, not merely the malformation -- "
           "the next reader has to know why a skip was not good enough";
    EXPECT_TRUE(sawDetail(rep, "D-LK-COFF-NAMELESS-UNDEF-EXTERN-SILENTLY-DROPPED"));
}

// The same arm serves class-STATIC records, and it must: `roleForStorageClass`
// resolves both EXTERNAL and STATIC before the UNDEF test, so a nameless STATIC
// record with SectionNumber 0 reaches exactly the same line and occupies
// exactly the same addressable slot.
TEST(CoffObjectReader, NamelessUndefStaticRecordIsRefusedByTheSameArm) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x10, 0x90u), {}}},
        {BSym{"fn", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"", 0, 0, 0, kClassStatic, std::nullopt, std::nullopt}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_CorruptedBinary));
    EXPECT_TRUE(sawDetail(rep, "D-LK-COFF-NAMELESS-UNDEF-EXTERN-SILENTLY-DROPPED"));
}

// ANTI-SUBSUMPTION, and it is the arm that keeps the refusal NARROW. A NAMED
// UNDEF extern -- the shape every real object is full of -- must still read
// green and still become an import. Without this, a reader that had started
// refusing every UNDEF record would satisfy both arms above.
TEST(CoffObjectReader, ANamedUndefExternStillReadsGreenAsAnImport) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const obj = buildCoff(
        {BSec{".text", kScnText, std::vector<std::uint8_t>(0x10, 0x90u), {}}},
        {BSym{"fn", 0, 1, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt},
         BSym{"puts", 0, 0, kDtypeFunction, kClassExternal, std::nullopt, std::nullopt}});
    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errs=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_TRUE(hasExternNamed(*got, "puts"))
        << "the named UNDEF record is still an extern import -- the refusal is "
           "about the MISSING NAME, not about UNDEF records";
}
