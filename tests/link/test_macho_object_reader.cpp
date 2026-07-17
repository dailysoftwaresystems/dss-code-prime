// Mach-O 64-bit MH_OBJECT relocatable-object MEMBER READER tests -- cycle
// c168, anchor D-LK-RELOCATABLE-OBJECT-READER-MACHO.
//
// The reader (`src/link/format/macho_object_reader.cpp`) is the INVERSE of
// macho.cpp's MH_OBJECT writer: it reconstructs a relocatable object's FULL
// linkable body back into an `AssembledModule` -- the exact structure the
// c154 cross-CU merge consumes -- the Mach-O sibling of the c164 ELF reader,
// unblocking the c165 static-link for Apple `.a` members.
//
// Coverage:
//   1. DSS writer <-> reader FULL-object ROUND-TRIP (the self-contained
//      oracle, arm64): write a module with 2 functions + rodata/data/relro
//      data + extern function + extern data + relocations, read it back,
//      assert every field class matches (function names + byte ranges sliced
//      by n_value, data sections + bytes, relocation {offset, target-by-name,
//      kind, addend}, extern isData inferred from the call-vs-address reloc).
//      Red-on-disable is inherent per field class (drop the reloc parse ->
//      relocations empty; drop the nlist slice -> functions unnamed).
//   2. Truncation-at-every-length fuzz -> every proper prefix fails loud
//      (nullopt + diagnostic), never crashes.
//   3. Corruption red-pins: bad magic; MH_EXECUTE filetype; unknown reloc
//      nativeId; r_extern=0 section-relative reloc -> all fail loud.
//   4. Non-Mach-O format schema -> fail loud.
//   5. x86_64 leaf-only agnosticism round-trip (the shipped
//      macho64-x86_64-darwin object format is text-only) -- the SAME reader
//      reconstructs it, proving no arm64 identity is baked in.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho.hpp"
#include "link/format/macho_object_reader.hpp"
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
[[nodiscard]] std::uint32_t rd32(std::vector<std::uint8_t> const& b, std::size_t o) {
    return  static_cast<std::uint32_t>(b[o])
         | (static_cast<std::uint32_t>(b[o + 1]) <<  8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16)
         | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

// A minimal valid arm64 MH_OBJECT: one function whose only instruction is a
// BL patched by a BRANCH26 relocation to an extern. Text-only, so the layout
// is fixed: header@0 (32) | LC_SEGMENT_64@32 (72 hdr + one 80-byte
// section_64 @104) | LC_SYMTAB@184 (24) | __text bytes | __text reloc table.
// The single section_64's reloff/nreloc live at file offsets 160/164.
[[nodiscard]] std::vector<std::uint8_t> validArm64Object(Loaded const& loaded) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x00, 0x00, 0x00, 0x94};   // BL #0 (patched by the reloc)
    fn.relocations.push_back(Relocation{0u, SymbolId{2}, RelocationKind{1}, 0}); // BRANCH26 -> g
    mod.functions.push_back(std::move(fn));
    mod.symbols = {ModuleSymbol{SymbolId{1}, "f", SymbolBinding::Global,
                                SymbolVisibility::Default}};
    mod.externImports = {ExternImport{SymbolId{2}, "g", "/usr/lib/libSystem.B.dylib", false}};
    DiagnosticReporter rep;
    return macho::encode(mod, *loaded.target, *loaded.format, rep);
}

// File offset of the single __text section_64's reloff field.
constexpr std::size_t kTextReloffField = 160;

} // namespace

// -- 1. DSS writer <-> reader full-object round-trip (arm64) ---------

TEST(MachoObjectReader, DssWriterRoundTripReconstructsEveryFieldClass) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    // A module exercising every reconstructable field class:
    //   * add     -- a leaf function (no relocations).
    //   * greet   -- a function with THREE __text relocations: a BRANCH26
    //                CALL to an extern FUNCTION (puts), a PAGE21 to a DEFINED
    //                rodata object (msg), and a PAGE21 to an extern DATA
    //                object (env). The reader must map each back to its kind
    //                and infer isData from the call-vs-address distinction.
    //   * msg     -- a Rodata data item (__TEXT,__const).
    //   * counter -- a Data data item (__DATA,__data).
    //   * vtable  -- a RelRoConst data item (__DATA,__const) carrying an
    //                UNSIGNED abs64 reloc to `add` with a NON-zero in-slot
    //                addend (exercises the data-slot addend read).
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction add;
    add.symbol = SymbolId{1};
    add.bytes  = {0xC0, 0x03, 0x5F, 0xD6};   // arm64 RET
    mod.functions.push_back(add);

    AssembledFunction greet;
    greet.symbol = SymbolId{2};
    greet.bytes.assign(12, 0x1F);            // 12 filler bytes; only the relocs matter
    greet.relocations.push_back(Relocation{0u, SymbolId{21}, RelocationKind{1}, 0}); // BRANCH26 -> puts
    greet.relocations.push_back(Relocation{4u, SymbolId{10}, RelocationKind{2}, 0}); // PAGE21   -> msg
    greet.relocations.push_back(Relocation{8u, SymbolId{20}, RelocationKind{2}, 0}); // PAGE21   -> env
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
    vtable.relocations.push_back(Relocation{0u, SymbolId{1}, RelocationKind{4}, 8}); // abs64 -> add, addend 8
    mod.dataItems.push_back(vtable);

    // All Global names round-trip verbatim through DSS's OWN writer (it emits
    // every defined symbol N_SECT|N_EXT and carves a LOCAL symbol's NAME to
    // `_sym_<id>`, so a Local name does not survive its own writer -- a writer
    // property, not a reader gap). We use Global names so every identity
    // round-trips (mirrors the ELF reader's round-trip discipline).
    mod.symbols = {
        ModuleSymbol{SymbolId{1},  "add",     SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{2},  "greet",   SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{10}, "msg",     SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{11}, "counter", SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{12}, "vtable",  SymbolBinding::Global, SymbolVisibility::Default},
    };
    mod.externImports = {
        ExternImport{SymbolId{20}, "env",  "/usr/lib/libSystem.B.dylib", /*isData=*/true},
        ExternImport{SymbolId{21}, "puts", "/usr/lib/libSystem.B.dylib", /*isData=*/false},
    };

    DiagnosticReporter wrep;
    auto objBytes = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u) << "writer must accept the module";
    ASSERT_FALSE(objBytes.empty());

    DiagnosticReporter rrep;
    auto readOpt = macho::readRelocatableObject(objBytes, *loaded.target,
                                                *loaded.format, rrep);
    ASSERT_TRUE(readOpt.has_value())
        << "reader must reconstruct the module (errors=" << rrep.errorCount() << ")";
    ASSERT_EQ(rrep.errorCount(), 0u);
    AssembledModule const& got = *readOpt;

    // -- functions: names + byte ranges (nlist n_value slicing) --
    ASSERT_EQ(got.functions.size(), 2u);
    auto const* rAdd = funcNamed(got, "add");
    auto const* rGreet = funcNamed(got, "greet");
    ASSERT_NE(rAdd, nullptr) << "add must be recovered by name (red-on-disable "
                               "vs a dropped nlist parse)";
    ASSERT_NE(rGreet, nullptr);
    EXPECT_EQ(rAdd->bytes, add.bytes) << "__text sliced by sorted n_value";
    EXPECT_EQ(rGreet->bytes, greet.bytes);
    EXPECT_TRUE(rAdd->relocations.empty());

    // -- __text relocations (offset relative to function start, kind mapped
    //    back from nativeId, addend 0, target by name) --
    ASSERT_EQ(rGreet->relocations.size(), 3u)
        << "all three __text relocs must land on greet "
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
    EXPECT_EQ(rPuts->kind, RelocationKind{1});   // BRANCH26
    EXPECT_EQ(rMsg->kind, RelocationKind{2});    // PAGE21
    EXPECT_EQ(rEnv->kind, RelocationKind{2});
    EXPECT_EQ(rPuts->addend, 0);                 // a __text reloc carries no addend
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
        << "msg resolves to __TEXT,__const via the (segment,name) pair";
    EXPECT_EQ(dMsg->bytes, msg.bytes);
    EXPECT_EQ(dCounter->section, DataSectionKind::Data);
    EXPECT_EQ(dCounter->bytes, counter.bytes);
    EXPECT_EQ(dVtable->section, DataSectionKind::RelRoConst)
        << "vtable resolves to __DATA,__const -- the SAME __const name as msg, "
           "distinguished ONLY by the __DATA segment";
    EXPECT_EQ(dVtable->bytes.size(), 8u);

    // -- data-item relocation: abs64 -> add, addend READ FROM THE SLOT --
    ASSERT_EQ(dVtable->relocations.size(), 1u)
        << "the relro item's own relocation must be recovered from its "
           "section reloc table";
    EXPECT_EQ(dVtable->relocations[0].offset, 0u);
    EXPECT_EQ(dVtable->relocations[0].kind, RelocationKind{4});   // abs64 UNSIGNED
    EXPECT_EQ(dVtable->relocations[0].addend, 8)
        << "Mach-O has no RELA addend column -- the addend must be recovered "
           "from the in-place slot bytes (red-on-disable vs a hardcoded 0)";
    EXPECT_EQ(dVtable->bytes[0], 8u)
        << "the writer baked the addend into the 8-byte slot; the reader "
           "reconstructs those literal bytes";
    EXPECT_EQ(nameOf(got, dVtable->relocations[0].target), "add");

    // -- extern imports: names + isData INFERENCE (call -> fn, address -> data) --
    auto const* ePuts = externNamed(got, "puts");
    auto const* eEnv = externNamed(got, "env");
    ASSERT_NE(ePuts, nullptr);
    ASSERT_NE(eEnv, nullptr);
    EXPECT_FALSE(ePuts->isData)
        << "puts is reached via a BRANCH26 call -> inferred a FUNCTION import";
    EXPECT_TRUE(eEnv->isData)
        << "env is reached via a PAGE21 address reloc -> inferred a DATA import";

    // -- the module is well-formed for the merge --
    EXPECT_EQ(got.expectedFuncCount, 2u);
}

// -- 1b. Multi-item-per-section slicing (VALUE correctness) ----------
//
// TWO named data items in ONE section (`__DATA,__data`) of differing
// alignment: the writer packs them with alignment PADDING and records each
// item's PADDED offset as its n_value. Since nlist_64 carries no size, the
// reader slices the earlier item [off_0, off_1) and ABSORBS the trailing
// inter-item padding into it (D-LK-MACHO-MULTI-ITEM-SECTION-PADDING). This
// test PINS that the absorption is VALUE-BENIGN, not a silent corruption:
// two atoms reconstruct (not one), each item's REAL content survives at
// offset 0 of its atom, and a reloc inside an item still routes to that item
// at the correct offset. It also locks the multi-item slice path the
// one-item-per-section round-trip above never exercises.
TEST(MachoObjectReader, MultiItemSectionSlicesEachAtomValueCorrect) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction f;
    f.symbol = SymbolId{1};
    f.bytes  = {0xC0, 0x03, 0x5F, 0xD6};   // arm64 RET (the abs64 reloc target)
    mod.functions.push_back(f);

    // d0 (4 bytes, align 4) then d1 (8 bytes, align 8) -- BOTH __DATA,__data,
    // so d1 lands at a padded offset after d0 and the reader slices d0 to
    // absorb the gap. d1 carries an abs64 reloc to f (routes across the slice).
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
    d1.relocations.push_back(Relocation{0u, SymbolId{1}, RelocationKind{4}, 0}); // abs64 -> f
    mod.dataItems.push_back(d1);

    mod.symbols = {
        ModuleSymbol{SymbolId{1},  "f",  SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{10}, "d0", SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{11}, "d1", SymbolBinding::Global, SymbolVisibility::Default},
    };

    DiagnosticReporter wrep;
    auto objBytes = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_FALSE(objBytes.empty());

    DiagnosticReporter rrep;
    auto got = macho::readRelocatableObject(objBytes, *loaded.target, *loaded.format, rrep);
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

    // d1's abs64 reloc routes to f at offset 0 -- reloc routing survives the
    // multi-item slice (red-on-disable vs a mis-attributed reloc).
    ASSERT_EQ(rd1->relocations.size(), 1u);
    EXPECT_EQ(rd1->relocations[0].offset, 0u);
    EXPECT_EQ(nameOf(*got, rd1->relocations[0].target), "f");
}

// -- 2. Truncation fuzz ----------------------------------------------

TEST(MachoObjectReader, TruncationAtEveryLengthFailsLoudNeverCrashes) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto full = validArm64Object(loaded);
    ASSERT_GT(full.size(), 32u);

    // Sanity: the full object reads back cleanly.
    {
        DiagnosticReporter rep;
        EXPECT_TRUE(macho::readRelocatableObject(full, *loaded.target, *loaded.format, rep)
                        .has_value());
    }
    // Every proper prefix must fail loud (nullopt + a diagnostic) -- the load
    // commands + section body + reloc table + symtab + strtab sit near EOF, so
    // any truncation makes some bounds check fire. Never a crash, never a
    // silent partial parse.
    for (std::size_t len = 1; len < full.size(); ++len) {
        std::vector<std::uint8_t> const trunc(full.begin(), full.begin() + len);
        DiagnosticReporter rep;
        auto got = macho::readRelocatableObject(trunc, *loaded.target, *loaded.format, rep);
        ASSERT_FALSE(got.has_value())
            << "truncation to " << len << " bytes must fail loud";
        EXPECT_GT(rep.errorCount(), 0u) << "a diagnostic must accompany the failure";
    }
}

// -- 3. Corruption red-pins ------------------------------------------

TEST(MachoObjectReader, BadMagicFailsLoud) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    auto obj = validArm64Object(loaded);
    obj[0] = 0x00;   // corrupt the magic (no longer 0xFEEDFACF)
    DiagnosticReporter rep;
    EXPECT_FALSE(macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_UnknownBinaryFormat) saw = true;
    EXPECT_TRUE(saw) << "a bad magic must emit F_UnknownBinaryFormat";
}

TEST(MachoObjectReader, MhExecuteFiletypeFailsLoud) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    auto obj = validArm64Object(loaded);
    obj[12] = 2;   // mach_header_64.filetype = MH_EXECUTE (a link OUTPUT)
    DiagnosticReporter rep;
    EXPECT_FALSE(macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_UnsupportedBinaryFormat) saw = true;
    EXPECT_TRUE(saw) << "an MH_EXECUTE filetype must emit F_UnsupportedBinaryFormat";
}

TEST(MachoObjectReader, UnknownRelocNativeIdFailsLoud) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    auto obj = validArm64Object(loaded);
    // Corrupt the __text reloc entry's r_type nibble (r_info bits 28..31) to a
    // value the format schema does not declare, KEEPING r_extern (bit 27) set
    // so the nativeId check -- not the r_extern check -- is what fires.
    std::uint32_t const reloff = rd32(obj, kTextReloffField);
    ASSERT_GT(reloff, 0u);
    ASSERT_LT(static_cast<std::size_t>(reloff) + 8u, obj.size());
    obj[reloff + 7] |= 0xF0u;   // r_type nibble -> 0xF (undeclared nativeId)
    DiagnosticReporter rep;
    EXPECT_FALSE(macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value())
        << "an undeclared reloc nativeId must not silently drop -- fail loud";
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_CorruptedBinary) saw = true;
    EXPECT_TRUE(saw);
}

TEST(MachoObjectReader, RExternZeroSectionRelocFailsLoud) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    auto obj = validArm64Object(loaded);
    // Clear the __text reloc entry's r_extern bit (bit 27 -> 0x08 in the high
    // r_info byte). DSS output is always symbol-relative (r_extern=1); an
    // r_extern=0 SECTION-INDEX reloc is the foreign-clang shape the reader
    // rejects (D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC).
    std::uint32_t const reloff = rd32(obj, kTextReloffField);
    ASSERT_GT(reloff, 0u);
    ASSERT_LT(static_cast<std::size_t>(reloff) + 8u, obj.size());
    obj[reloff + 7] &= static_cast<std::uint8_t>(~0x08u);   // clear r_extern
    DiagnosticReporter rep;
    EXPECT_FALSE(macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rep).has_value());
    bool sawCode = false;
    bool sawAnchor = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_UnsupportedBinaryFormat) sawCode = true;
        if (d.actual.find("D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC")
            != std::string::npos) {
            sawAnchor = true;
        }
    }
    EXPECT_TRUE(sawCode) << "an r_extern=0 reloc must emit F_UnsupportedBinaryFormat";
    EXPECT_TRUE(sawAnchor)
        << "the diagnostic must name the D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC "
           "follow-up anchor";
}

// -- 4. Non-Mach-O format schema -------------------------------------

TEST(MachoObjectReader, NonMachOFormatSchemaFailsLoud) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto obj = validArm64Object(loaded);
    // An ELF format schema cannot parse a Mach-O object.
    auto elf = ObjectFormatSchema::loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(elf.has_value());
    DiagnosticReporter rep;
    EXPECT_FALSE(macho::readRelocatableObject(obj, *loaded.target, **elf, rep).has_value());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::F_UnsupportedBinaryFormat) saw = true;
    EXPECT_TRUE(saw) << "an ELF schema must fail loud F_UnsupportedBinaryFormat";
}

// -- 5. x86_64 leaf-only agnosticism ---------------------------------
//
// AGNOSTICISM: the shipped macho64-x86_64-darwin object format is text-only
// (leaf-only -- no data-section rows, no runtime leg). The SAME reader
// reconstructs an x86_64 leaf function via the x86_64 FORMAT schema (whose
// X86_64_RELOC_BRANCH nativeId differs from arm64's, and whose section table
// carries only __text) -- no hardcoded arm64 / __const / cpu identity in the
// reader. Red-on-disable vs a machine branch.

TEST(MachoObjectReader, X86_64LeafRoundTripIsMachineAgnostic) {
    auto loaded = loadShipped("x86_64", "macho64-x86_64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};   // x86_64 RET (a leaf, no relocations)
    mod.functions.push_back(fn);
    mod.symbols = {ModuleSymbol{SymbolId{1}, "leaf", SymbolBinding::Global,
                                SymbolVisibility::Default}};

    DiagnosticReporter wrep;
    auto objBytes = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_FALSE(objBytes.empty());

    DiagnosticReporter rrep;
    auto got = macho::readRelocatableObject(objBytes, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value()) << "x86_64 leaf object must reconstruct (errors="
                                 << rrep.errorCount() << ")";
    ASSERT_EQ(rrep.errorCount(), 0u);
    ASSERT_EQ(got->functions.size(), 1u);
    auto const* leaf = funcNamed(*got, "leaf");
    ASSERT_NE(leaf, nullptr) << "the leaf function must be recovered by name";
    EXPECT_EQ(leaf->bytes, fn.bytes) << "__text sliced by n_value (single atom)";
    EXPECT_TRUE(leaf->relocations.empty());
    EXPECT_TRUE(got->dataItems.empty());
    EXPECT_TRUE(got->externImports.empty());
}
