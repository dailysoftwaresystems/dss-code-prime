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
//   5. x86_64 agnosticism -- the SAME reader reconstructs an x86_64 object,
//      proving no arm64 identity is baked in: (5) a leaf round-trip, then
//      (5b/5c) the extern-CLASS half, which is where the reader used to be
//      wrong. An extern's function-vs-data class is read from the FORMAT
//      row's declared `isCall` role, pinned in BOTH directions in one object,
//      and a format that omits the declaration REFUSES instead of guessing
//      (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL).
//   6. WHICH DEFINED SYMBOLS START AN ATOM -- MH_SUBSECTIONS_VIA_SYMBOLS in
//      the mach_header paired with N_ALT_ENTRY in n_desc, pinned in all three
//      directions (local without alt-entry -> its own atom; the same local
//      with alt-entry -> an interior label absorbed by its enclosing atom;
//      no header flag -> undecidable, and the shared coverage guard refuses
//      loud rather than dropping the body).
//      D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM.
//   7. The same classification against REAL clang-produced bytes (a
//      checked-in `.o` carrying a file-local `static` function AND an
//      `.alt_entry` symbol) -- the foreign witness that (6) reads the
//      FORMAT's vocabulary and not a DSS convention.
//   8. EQUAL-OFFSET SYMBOLS ARE ONE ATOM UNDER SEVERAL NAMES, against a
//      second REAL clang `.o` whose section-start label shares an offset with
//      the first function AND whose relocation lands in the span they share --
//      the routing property, not just the count. Plus the TARGET half (a
//      relocation naming an alias binds to the atom that owns the body).
//      D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/object_format_schema.hpp"

#include "clang_macho_equal_offset_label_object.inc"
#include "clang_macho_subsections_object.inc"
#include "repo_root.hpp"   // the ONE test-side repo/config-root resolver

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>   // the "shipped file MINUS one key" fixture (5c)

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

// -- 5. x86_64 leaf agnosticism --------------------------------------
//
// AGNOSTICISM: the SAME reader reconstructs an x86_64 leaf function via the
// x86_64 FORMAT schema -- no hardcoded arm64 / __const / cpu identity in the
// reader. Red-on-disable vs a machine branch.
//
// ⚠ THIS COMMENT USED TO SAY the shipped macho64-x86_64-darwin format was
// "text-only (leaf-only -- no data-section rows, no runtime leg)". All three
// clauses are FALSE and were already false when this was read: the document
// declares __text/__const/__data/__const/__bss rows, declares
// `externCallDispatch: direct-plt`, and the operator's Mac runs the
// macho64-x86_64 leg under Rosetta. The test itself is unchanged and still
// exercises a leaf; only the claim about the FORMAT was wrong. Cases 5b/5c
// below are the non-leaf half it wrongly implied did not exist.

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

// -- 5b. AN EXTERN'S CLASS COMES FROM THE FORMAT'S DECLARED `isCall` ROLE,
//        NOT FROM THE TARGET'S ARITHMETIC FORMULA ---------------------
//
// D-LK-MACHO-ISDATA-NO-CALL-SIGNAL. Mach-O's nlist_64 carries no STT_FUNC
// -style type hint, so the ONLY thing that can tell an undefined `_puts` from
// an undefined `_environ` is the relocation that reaches it. The reader used
// to ask the TARGET row's `formulaKind`, which is a proxy for the wrong
// thing: a formula describes ARITHMETIC, and this question is about ROLE. It
// held on arm64 by accident (that CPU's branch has its own encoding, so
// `aarch64_call26` happens to be branch-specific) and failed on x86_64 BY
// CONSTRUCTION -- `S + A - P` is the identical arithmetic for a call and for
// a PC-relative data reference, so `linear` is the honest formula and says
// nothing about role.
//
// ✔MEASURED 2026-08-20 through the shipped CLI, before the reader changed:
// `puts("x")` in an archive member built `--target x86_64:macho64-x86_64-
// darwin-staticlib` compiled rc=0, and resolving it into a client rc=1 with
// `error[F_UnsupportedBinaryFormat] ... declares no call-branch-formula
// relocation`, while the arm64 sibling compiled the same two sources rc=0
// twice. i.e. macho64-x86_64 had NO working static-library path at all.
//
// BOTH DIRECTIONS IN ONE OBJECT, and the SECOND is the half that keeps the
// fix honest: "every reached extern is a function" would pass the BRANCH case
// and silently mis-type every extern DATA object. Both relocations sit in
// `__text`, so the ONLY variable between them is the relocation kind.
TEST(MachoObjectReader, X86_64ExternClassComesFromTheDeclaredCallRole) {
    auto loaded = loadShipped("x86_64", "macho64-x86_64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes.assign(16, 0x90);   // filler; only the relocations matter here
    // kind 1 = x86_64.target.json `rel32` -> X86_64_RELOC_BRANCH, the row the
    // format declares `"isCall": true` on.
    fn.relocations.push_back(Relocation{0u, SymbolId{20}, RelocationKind{1}, 0});
    // kind 2 = `abs64` -> X86_64_RELOC_UNSIGNED_8, an address reloc with NO
    // declared call role.
    fn.relocations.push_back(Relocation{8u, SymbolId{21}, RelocationKind{2}, 0});
    mod.functions.push_back(fn);
    mod.symbols = {ModuleSymbol{SymbolId{1}, "caller", SymbolBinding::Global,
                                SymbolVisibility::Default}};
    // Both are seeded isData=TRUE on the way in, so a reader that simply
    // preserved the writer's value could not pass the first assertion, and a
    // reader that forced every reached extern to a function could not pass the
    // second. The seed is not the answer in either direction.
    mod.externImports = {
        ExternImport{SymbolId{20}, "puts", "/usr/lib/libSystem.B.dylib", /*isData=*/true},
        ExternImport{SymbolId{21}, "env",  "/usr/lib/libSystem.B.dylib", /*isData=*/true},
    };

    DiagnosticReporter wrep;
    auto objBytes = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u) << "writer must accept the module";
    ASSERT_FALSE(objBytes.empty());

    DiagnosticReporter rrep;
    auto got = macho::readRelocatableObject(objBytes, *loaded.target,
                                            *loaded.format, rrep);
    ASSERT_TRUE(got.has_value())
        << "an x86_64 Mach-O object whose extern is reached by a relocation "
           "must READ BACK -- this is the repro that had no working static "
           "library path at all (errors=" << rrep.errorCount() << ")";
    ASSERT_EQ(rrep.errorCount(), 0u);

    auto const* ePuts = externNamed(*got, "puts");
    auto const* eEnv  = externNamed(*got, "env");
    ASSERT_NE(ePuts, nullptr);
    ASSERT_NE(eEnv, nullptr);
    EXPECT_FALSE(ePuts->isData)
        << "reached by X86_64_RELOC_BRANCH, which macho64-x86_64-darwin "
           "declares `\"isCall\": true` on -> a FUNCTION import. The target's "
           "formula for this kind is `linear`, so a formula-derived answer "
           "cannot get here";
    EXPECT_TRUE(eEnv->isData)
        << "reached by X86_64_RELOC_UNSIGNED_8, which declares no call role "
           "-> the DATA seed stands. Without this half, 'every reached extern "
           "is a function' would also be green";
}

// -- 5c. A MACH-O FORMAT THAT FORGETS THE DECLARATION REFUSES, NEVER
//        GUESSES -------------------------------------------------------
//
// The `callSignalNativeIds.empty()` arm of the reader. It is what every
// macho64-x86_64 member hit before 2026-08-20; what changed is WHO can reach
// it -- no shipped Mach-O format does any more, and it now guards the case it
// should always have guarded: a NEW Mach-O format that omits `isCall`.
//
// ★ THE FIXTURE IS THE SHIPPED FILE MINUS EXACTLY ONE KEY (the
// `test_c_symbol_decoration.cpp` discipline), so it cannot drift away from the
// document it is standing in for, and the CONTROL below proves the unmodified
// document reads the SAME BYTES clean -- without which the refusal would prove
// nothing about the erased key.
TEST(MachoObjectReader, X86_64FormatMinusItsIsCallDeclarationRefusesRatherThanGuesses) {
    auto const root = dss::test::findConfigRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::configRootDiagnostic();
    auto const leaf = *root / "object-formats"
                    / "macho64-x86_64-darwin.format.json";
    std::ifstream in{leaf, std::ios::binary};
    ASSERT_TRUE(in.good()) << "cannot open " << leaf.string();
    std::string const text{std::istreambuf_iterator<char>{in},
                           std::istreambuf_iterator<char>{}};
    ASSERT_FALSE(text.empty());

    auto loaded = loadShipped("x86_64", "macho64-x86_64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    // One object, read twice through two schemas that differ in ONE key.
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes.assign(8, 0x90);
    fn.relocations.push_back(Relocation{0u, SymbolId{20}, RelocationKind{1}, 0});
    mod.functions.push_back(fn);
    mod.symbols = {ModuleSymbol{SymbolId{1}, "caller", SymbolBinding::Global,
                                SymbolVisibility::Default}};
    mod.externImports = {
        ExternImport{SymbolId{20}, "puts", "/usr/lib/libSystem.B.dylib", true}};
    DiagnosticReporter wrep;
    auto const objBytes = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_FALSE(objBytes.empty());

    // CONTROL: the unmodified shipped document loads AND reads these bytes.
    auto control = ObjectFormatSchema::loadFromText(text, "macho64-x86_64-darwin");
    ASSERT_TRUE(control.has_value())
        << "the unmodified shipped format must load, or the refusal below "
           "proves nothing about the erased key";
    {
        DiagnosticReporter rep;
        auto ok = macho::readRelocatableObject(objBytes, *loaded.target,
                                               **control, rep);
        ASSERT_TRUE(ok.has_value())
            << "control: WITH the declaration, these exact bytes read clean";
        EXPECT_EQ(rep.errorCount(), 0u);
    }

    // Erase exactly the `isCall` declaration -- an in-memory copy; the file on
    // disk is never reserialized.
    nlohmann::json doc = nlohmann::json::parse(text);
    bool erased = false;
    for (auto& row : doc.at("relocations")) {
        if (row.contains("isCall")) { row.erase("isCall"); erased = true; }
    }
    ASSERT_TRUE(erased)
        << "macho64-x86_64-darwin must declare isCall on its BRANCH row -- if "
           "this fires, the declaration was dropped and 5b is the live pin";
    auto stripped = ObjectFormatSchema::loadFromText(doc.dump(),
                                                     "macho64-x86_64-noiscall");
    ASSERT_TRUE(stripped.has_value())
        << "the key is OPTIONAL at load -- its absence is a fact about the "
           "format, not a malformed document";

    DiagnosticReporter rep;
    auto refused = macho::readRelocatableObject(objBytes, *loaded.target,
                                                **stripped, rep);
    EXPECT_FALSE(refused.has_value())
        << "a Mach-O format declaring no isCall row cannot classify an extern "
           "it meets only as a name; DATA would be a SILENT GUESS";
    bool named = false;
    bool rightCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_UnsupportedBinaryFormat) rightCode = true;
        if (d.actual.find("D-LK-MACHO-ISDATA-NO-CALL-SIGNAL") != std::string::npos
            && d.actual.find("isCall") != std::string::npos) named = true;
    }
    EXPECT_TRUE(rightCode);
    EXPECT_TRUE(named)
        << "the refusal must name the missing SCHEMA KEY and the anchor -- "
           "adding that key is the whole fix, so a message that does not say "
           "so sends the next maintainer into the reader instead";
}

// -- 5d. AN EMISSION ALIAS IS A FORMAT-AGNOSTIC ESCAPE HATCH ----------
//
// `emitOnly` states that one WIRE type carries two DSS patch-site semantics,
// so the emitter can reach it through a second `kind` while the DECODER keeps
// exactly one answer per wire id. `elf_object_reader.cpp` excluded alias rows
// from its reverse map; this reader's copy of that loop did NOT, and the
// difference was invisible because no shipped Mach-O document declares one.
//
// It was never a miscompile, and this pin does not claim it was. Every valid
// document has UNIQUE `kind`s (`validateRelocationsTable`), so an alias always
// carries a different kind from the row owning its wire id, so the old loop
// would have hit its own ambiguity refusal -- LOUD, and refusing every object
// of the format. What was missing was the CAPABILITY, on two of three formats.
//
// The fix is not a second `if (r.emitOnly) continue;`: it is that no reader
// owns that loop any more (`ObjectFormatSchema::relocationDecodeTable`). This
// pin is therefore also the Mach-O witness that the shared builder is wired in.
//
// RED-ON-DISABLE: delete the `if (r.emitOnly) continue;` in
// `relocationDecodeTable` and the spliced read below is refused with
// "ambiguous reverse map" (this test + the COFF and ELF twins go red).
TEST(MachoObjectReader, EmitOnlyAliasIsHonouredNotRefused) {
    auto const root = dss::test::findConfigRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::configRootDiagnostic();
    std::string text;
    {
        std::ifstream in{*root / "object-formats"
                             / "macho64-x86_64-darwin.format.json",
                         std::ios::binary};
        ASSERT_TRUE(in.good());
        text.assign(std::istreambuf_iterator<char>{in},
                    std::istreambuf_iterator<char>{});
    }
    ASSERT_FALSE(text.empty());
    ASSERT_EQ(text.find("\"emitOnly\""), std::string::npos)
        << "no shipped Mach-O document declares an emission alias -- if one "
           "now does, this fixture is no longer the 'plus one row' it claims "
           "to be, and the gap it pins was no longer latent";

    auto loaded = loadShipped("x86_64", "macho64-x86_64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    // One object, read twice through two schemas differing in ONE ROW. Its
    // single relocation uses kind 1 -- the row the alias will shadow.
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes.assign(8, 0x90);
    fn.relocations.push_back(Relocation{0u, SymbolId{20}, RelocationKind{1}, 0});
    mod.functions.push_back(fn);
    mod.symbols = {ModuleSymbol{SymbolId{1}, "caller", SymbolBinding::Global,
                                SymbolVisibility::Default}};
    mod.externImports = {
        ExternImport{SymbolId{20}, "puts", "/usr/lib/libSystem.B.dylib", true}};
    DiagnosticReporter wrep;
    auto const objBytes = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_FALSE(objBytes.empty());

    // CONTROL: unmodified, these bytes decode to kind 1 and classify `puts` as
    // a FUNCTION. Without this the assertion below could not be attributed to
    // the added row.
    {
        auto control = ObjectFormatSchema::loadFromText(text,
                                                        "macho64-x86_64-darwin");
        ASSERT_TRUE(control.has_value());
        DiagnosticReporter rep;
        auto got = macho::readRelocatableObject(objBytes, *loaded.target,
                                                **control, rep);
        ASSERT_TRUE(got.has_value()) << "errors=" << rep.errorCount();
        ASSERT_EQ(got->functions.size(), 1u);
        ASSERT_EQ(got->functions[0].relocations.size(), 1u);
        EXPECT_EQ(got->functions[0].relocations[0].kind.v, 1u);
    }

    // The SAME document plus exactly one row: an alias of whatever wire id the
    // kind-1 row owns, carrying the first kind nothing else claims. Both
    // numbers are DERIVED from the document -- a hardcoded packed Mach-O
    // r_info or a hardcoded kind would go stale silently.
    nlohmann::json doc = nlohmann::json::parse(text);
    std::uint32_t ownerNativeId = 0;
    std::uint32_t aliasKind     = 0;
    {
        std::vector<std::uint32_t> taken;
        for (auto const& row : doc.at("relocations")) {
            auto const k = row.at("kind").get<std::uint32_t>();
            taken.push_back(k);
            if (k == 1u) ownerNativeId = row.at("nativeId").get<std::uint32_t>();
        }
        ASSERT_NE(ownerNativeId, 0u)
            << "the kind-1 row moved or was renumbered -- re-derive this fixture";
        for (std::uint32_t k = 1; k < 64u && aliasKind == 0u; ++k) {
            if (std::ranges::find(taken, k) == taken.end()) aliasKind = k;
        }
        ASSERT_NE(aliasKind, 0u);
    }
    doc.at("relocations").push_back(nlohmann::json{
        {"name", "X86_64_RELOC_BRANCH_EMIT_ALIAS"},
        {"kind", aliasKind},
        {"nativeId", ownerNativeId},
        {"emitOnly", true}});

    auto aliased = ObjectFormatSchema::loadFromText(doc.dump(),
                                                     "macho64-x86_64-alias");
    ASSERT_TRUE(aliased.has_value())
        << "an emission alias is a SCHEMA-level shape, not an ELF one -- "
           "`validate()` must accept it for every format";

    DiagnosticReporter rep;
    auto got = macho::readRelocatableObject(objBytes, *loaded.target,
                                            **aliased, rep);
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
    auto const* ep = externNamed(*got, "puts");
    ASSERT_NE(ep, nullptr);
    EXPECT_FALSE(ep->isData)
        << "the call signal belongs to the OWNING row and must survive the "
           "alias -- `isCall` is read through the very map the alias is "
           "excluded from (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL)";
}


// -- 6. MH_SUBSECTIONS_VIA_SYMBOLS + N_ALT_ENTRY: which defined symbols
//       START AN ATOM -----------------------------------------------
//
// D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM. Mach-O's
// nlist_64 has NO size field, so a whole file-local (`static`) FUNCTION and an
// interior `&&label` block symbol are the same three numbers -- bare N_SECT, a
// section ordinal, an offset. This reader used to answer "does it start a
// body?" with N_EXT, which is right for the label and WRONG for the function:
// every `static` function pulled from an archive lost its bytes (loudly when
// called, SILENTLY when not).
//
// The format answers it with a PAIR of its own fields, and these tests pin both
// halves and the interaction:
//   * the header's MH_SUBSECTIONS_VIA_SYMBOLS (the producer's declaration that
//     its symbols carve the section into independent blocks), and
//   * per-symbol N_ALT_ENTRY in n_desc (the exception: "I am an alternate entry
//     into the atom before me").
//
// The fixture is DSS's OWN writer output, which is the honest oracle here
// because the write half of this same anchor is what makes those bytes carry
// the pair at all: the object format schema declares flags = 0x2000 and the
// MH_OBJECT walker stamps N_ALT_ENTRY on every synthetic per-block label. A
// module with a GLOBAL function, a LOCAL (`static`) function and a BLOCK label
// therefore produces all three shapes in one object. The checked-in clang
// object (`clang_macho_subsections_object.inc`, used in section 7 below) is the
// independent foreign witness that the pair is the FORMAT's vocabulary and not
// a DSS convention.

namespace {

// nlist_64 field offsets within one 16-byte record.
constexpr std::size_t   kNlistTypeOff = 4;
constexpr std::size_t   kNlistDescOff = 6;
constexpr std::size_t   kNlistSize    = 16;
constexpr std::uint16_t kNAltEntry    = 0x0200;   // N_ALT_ENTRY
constexpr std::size_t   kHdrFlagsOff  = 24;       // mach_header_64.flags
constexpr std::uint32_t kMhSubsections = 0x2000;  // MH_SUBSECTIONS_VIA_SYMBOLS

// A module with THREE `__text` shapes whose nlists differ only in the bits this
// anchor is about: an externally-visible function, a file-local one (the writer
// renames it `_sym_11` and drops N_EXT), and a synthetic per-block label inside
// the LOCAL one. Distinct byte counts make each atom identifiable by SIZE, so
// the assertions never depend on symbol ORDER.
//
// ⚠ `localFirst` CHOOSES WHICH SIDE OF A KNOWN GUARD BOUNDARY THE FIXTURE SITS
// ON, and it is not a stylistic knob. The last atom in a section runs to the
// section END (nlist_64 has no size), so a demoted local that TRAILS a
// surviving atom has its offset covered by that atom and the shared coverage
// guard cannot see it -- the boundary `object_atom_coverage.hpp` states in its
// own docblock. A test that wants to WITNESS the guard must therefore put the
// local FIRST (localFirst = true), where nothing covers it; a test that wants
// an alternate entry ABSORBED must put it AFTER (localFirst = false). ✔The
// first draft of the no-flag test got this wrong and read clean.
[[nodiscard]] AssembledModule threeShapeModule(bool localFirst) {
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction pub;                        // 4 bytes: arm64 RET
    pub.symbol = SymbolId{10};
    pub.bytes  = {0xC0, 0x03, 0x5F, 0xD6};

    AssembledFunction loc;                        // 8 bytes: RET ; RET
    loc.symbol = SymbolId{11};
    loc.bytes  = {0xC0, 0x03, 0x5F, 0xD6, 0xC0, 0x03, 0x5F, 0xD6};
    loc.blockSymbols.push_back({SymbolId{77}, /*blockByteOffset=*/4u});

    if (localFirst) {
        mod.functions.push_back(std::move(loc));
        mod.functions.push_back(std::move(pub));
    } else {
        mod.functions.push_back(std::move(pub));
        mod.functions.push_back(std::move(loc));
    }

    mod.symbols = {
        ModuleSymbol{SymbolId{10}, "_realfn", SymbolBinding::Global,
                     SymbolVisibility::Default},
        // `static` in the source: the writer carves the NAME to `_sym_11` and
        // emits bare N_SECT (D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION),
        // which is precisely why the reader cannot recover it by
        // name or by binding and must read the wire fields instead.
        ModuleSymbol{SymbolId{11}, "_statfn", SymbolBinding::Local,
                     SymbolVisibility::Default},
    };
    return mod;
}

// LC_SYMTAB lives at a fixed offset for this single-section layout: header(32)
// + LC_SEGMENT_64(72) + one section_64(80) + LC_BUILD_VERSION(24) = 208.
constexpr std::size_t kSymtabCmdOff = 208;

struct SymtabLoc { std::size_t symoff; std::uint32_t nsyms; std::size_t stroff; };

[[nodiscard]] SymtabLoc symtabOf(std::vector<std::uint8_t> const& obj) {
    return SymtabLoc{rd32(obj, kSymtabCmdOff + 8), rd32(obj, kSymtabCmdOff + 12),
                     rd32(obj, kSymtabCmdOff + 16)};
}

// Byte offset of the nlist_64 whose string-table name is `want`, or SIZE_MAX.
[[nodiscard]] std::size_t nlistOffsetOfName(std::vector<std::uint8_t> const& obj,
                                            SymtabLoc const& st,
                                            std::string const& want) {
    for (std::uint32_t i = 0; i < st.nsyms; ++i) {
        std::size_t const o = st.symoff + static_cast<std::size_t>(i) * kNlistSize;
        std::uint32_t const strx = rd32(obj, o);
        std::string name;
        for (std::size_t p = st.stroff + strx; p < obj.size() && obj[p] != 0; ++p) {
            name.push_back(static_cast<char>(obj[p]));
        }
        if (name == want) return o;
    }
    return static_cast<std::size_t>(-1);
}

[[nodiscard]] AssembledFunction const* funcOfSize(AssembledModule const& m,
                                                  std::size_t n) {
    for (auto const& f : m.functions) if (f.bytes.size() == n) return &f;
    return nullptr;
}

[[nodiscard]] ModuleSymbol const* symNamed(AssembledModule const& m,
                                           std::string const& name) {
    for (auto const& s : m.symbols) if (s.name == name) return &s;
    return nullptr;
}

} // namespace

// A LOCAL defined symbol with NO N_ALT_ENTRY, in an object that declares
// MH_SUBSECTIONS_VIA_SYMBOLS, is its OWN ATOM -- with its BYTES.
//
// This is the whole defect in one assertion: `_sym_11` is a `static` function,
// it is not external, and before this it came back as a bodiless ModuleSymbol
// while its 8 bytes reached nothing.
//
// RED-ON-DISABLE (watched, not read): make the reader ignore
// MH_SUBSECTIONS_VIA_SYMBOLS (`startsAtom = isExt`) and `_sym_11` is demoted
// again -- the shared coverage guard then REFUSES the whole read
// (F_ObjectReaderSymbolBodyDropped, nullopt), so ASSERT_TRUE on the read result
// goes red first and the function-count / binding assertions with it.
TEST(MachoObjectReader, SubsectionsLocalWithoutAltEntryBecomesItsOwnAtom) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter wrep;
    // Local FIRST -- the same geometry the no-flag test below uses, so the two
    // differ in exactly ONE header bit and nothing else.
    AssembledModule mod = threeShapeModule(/*localFirst=*/true);
    auto obj = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_FALSE(obj.empty());

    // The written object must actually carry the pair -- otherwise this test
    // would be asserting the reader against a premise that had silently gone
    // false (the write half is a config value plus one n_desc field, and either
    // can be reverted without touching this file).
    ASSERT_EQ(rd32(obj, kHdrFlagsOff) & kMhSubsections, kMhSubsections)
        << "the shipped macho64-arm64-darwin schema must declare "
           "MH_SUBSECTIONS_VIA_SYMBOLS -- without it this test proves nothing";

    DiagnosticReporter rrep;
    auto got = macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value())
        << "reader must reconstruct the object (errors=" << rrep.errorCount() << ")";
    ASSERT_EQ(rrep.errorCount(), 0u);

    // TWO atoms -- the global AND the file-local one. The block label is not a
    // third: it is interior to the local function.
    ASSERT_EQ(got->functions.size(), 2u)
        << "a file-local function must be an atom of its own, and a block label "
           "must not be";

    auto const* pub = funcOfSize(*got, 4);
    auto const* loc = funcOfSize(*got, 8);
    ASSERT_NE(pub, nullptr) << "the 4-byte global function must be recovered";
    ASSERT_NE(loc, nullptr)
        << "THE DEFECT: the 8-byte file-local function's BYTES must be "
           "recovered, not dropped to a bodiless ModuleSymbol";
    EXPECT_EQ(nameOf(*got, loc->symbol), "_sym_11");
    EXPECT_EQ(loc->bytes,
              (std::vector<std::uint8_t>{0xC0, 0x03, 0x5F, 0xD6,
                                         0xC0, 0x03, 0x5F, 0xD6}))
        << "the local atom must carry BOTH of its instructions -- a short slice "
           "would mean the block label had split it";

    // -- BINDING: Local, and this is load-bearing, not decorative --
    // `resolveCrossCuDefs` skips Local defs ("module-private -- excluded"), so
    // this is what stops one TU's `static` helper from satisfying another TU's
    // extern. Global would make two members' identically-named helpers a
    // duplicate-definition conflict; Weak would let one member's body be
    // silently dropped as a shadowed duplicate.
    auto const* locSym = symNamed(*got, "_sym_11");
    ASSERT_NE(locSym, nullptr);
    EXPECT_EQ(locSym->binding, SymbolBinding::Local)
        << "a promoted file-local atom must stay module-private -- a `static` "
           "function must never satisfy another TU's extern";
    auto const* pubSym = symNamed(*got, "_realfn");
    ASSERT_NE(pubSym, nullptr);
    EXPECT_EQ(pubSym->binding, SymbolBinding::Global)
        << "an externally-visible atom must stay Global";

    // The block label survives as a bodiless LOCAL symbol -- an identity a
    // relocation can name, never an atom.
    auto const* blockSym = symNamed(*got, "_sym_77");
    ASSERT_NE(blockSym, nullptr) << "the interior block label must still be a "
                                   "resolvable identity";
    EXPECT_EQ(blockSym->binding, SymbolBinding::Local);
    for (auto const& f : got->functions) {
        EXPECT_NE(nameOf(*got, f.symbol), "_sym_77")
            << "an N_ALT_ENTRY block label must never become an atom";
    }
}

// The SAME object with N_ALT_ENTRY set on the file-local function: it is an
// interior label again, absorbed into the atom that contains its offset.
//
// This is the other direction of the same rule, and it is what proves the
// reader reads n_desc rather than merely having stopped reading N_EXT. Patching
// one bit of one nlist is the whole difference between the two tests.
//
// RED-ON-DISABLE (watched): drop the `altEntry` term from `startsAtom` and the
// local becomes an atom here too -- the function count goes 1 -> 2 and the
// absorbed-bytes assertion fails.
TEST(MachoObjectReader, SubsectionsLocalWithAltEntryStaysAnInteriorLabel) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter wrep;
    // Local SECOND: for an alternate entry to be ABSORBED there has to be an
    // atom before it to absorb it.
    AssembledModule mod = threeShapeModule(/*localFirst=*/false);
    auto obj = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);

    SymtabLoc const st = symtabOf(obj);
    std::size_t const locOff = nlistOffsetOfName(obj, st, "_sym_11");
    ASSERT_NE(locOff, static_cast<std::size_t>(-1));
    // Confirm the starting state before mutating it: bare N_SECT, n_desc = 0.
    ASSERT_EQ(obj[locOff + kNlistTypeOff], 0x0Eu);
    ASSERT_EQ(obj[locOff + kNlistDescOff], 0u);
    ASSERT_EQ(obj[locOff + kNlistDescOff + 1], 0u);

    obj[locOff + kNlistDescOff]     = static_cast<std::uint8_t>(kNAltEntry & 0xFF);
    obj[locOff + kNlistDescOff + 1] = static_cast<std::uint8_t>(kNAltEntry >> 8);

    DiagnosticReporter rrep;
    auto got = macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value())
        << "an alternate entry point is a legal shape, not a corrupt object "
           "(errors=" << rrep.errorCount() << ")";
    ASSERT_EQ(rrep.errorCount(), 0u);

    // ONE atom now: the global function, which runs to the end of `__text` and
    // therefore CONTAINS the bytes the alternate entry points into. Nothing is
    // dropped -- which is exactly why the coverage guard stays silent here and
    // fires in the no-flag case below.
    ASSERT_EQ(got->functions.size(), 1u)
        << "an N_ALT_ENTRY symbol must not split the atom it lives in";
    EXPECT_EQ(nameOf(*got, got->functions[0].symbol), "_realfn");
    EXPECT_EQ(got->functions[0].bytes.size(), 12u)
        << "the surviving atom must absorb the alternate entry's bytes "
           "(4 + 8), not stop at the alt-entry offset";

    auto const* absorbed = symNamed(*got, "_sym_11");
    ASSERT_NE(absorbed, nullptr);
    EXPECT_EQ(absorbed->binding, SymbolBinding::Local);
    for (auto const& f : got->functions) {
        EXPECT_NE(nameOf(*got, f.symbol), "_sym_11");
    }
}

// WITHOUT the header flag the reader must NOT guess. The object makes no
// subsection claim, so a non-external defined symbol is genuinely ambiguous --
// and the shared coverage guard refuses rather than letting bytes vanish.
//
// This pins the boundary the read half deliberately does not cross: the fix is
// "honour what the producer declared", never "infer atoms from geometry". It
// also pins that the header flag is the thing doing the work -- these are the
// same bytes as the first test with one bit cleared.
//
// RED-ON-DISABLE (watched): make `startsAtom` ignore the flag and this object
// reads clean, so both the nullopt and the error-count assertions go red.
//
// ⚠ THIS PIN IS EXPECTED TO MOVE, AND THAT IS THE POINT OF WRITING IT DOWN. A
// GEOMETRY fallback -- promote an uncovered demoted symbol to an atom by
// looking at where the surrounding atoms are, without any format evidence --
// is being built in the shared coverage substrate for the readers whose format
// offers no declaration at all. If it comes to apply to Mach-O, this object
// stops being undecidable and this test SHOULD go red, because the refusal it
// pins will have been deliberately replaced by a recovery. Update it then;
// do not weaken it now in anticipation. What must NOT change either way is the
// alternative: silently dropping the body.
// An object that makes NO subsection claim: the wire says nothing, so GEOMETRY
// decides -- and it reaches the same answer the declaration would have.
//
// ⓘ THIS PIN USED TO ASSERT THE REFUSAL, and the operator overturned that
// 2026-08-20: the atom-coverage geometry fallback lands on all three readers
// (`uncoveredDefinedSymbolsThatStartAnAtom`). A local at offset 0 that no
// reconstructed atom covers cannot be interior to one, so it starts a body, and
// promoting it preserves bytes that refusing merely refused to lose.
//
// ★★ WHAT MAKES THIS THE INTERESTING ARM RATHER THAN A DUPLICATE OF THE ONE
// ABOVE: the object also carries a BLOCK LABEL at +4, stamped N_ALT_ENTRY by
// the writer. Geometry must NOT promote that one -- it is a per-symbol
// declaration, and an inference may not overrule a producer that spoke. So the
// correct answer here is TWO atoms and an absorbed label, byte-identical to the
// flagged read, reached by an entirely different route.
TEST(MachoObjectReader, NoSubsectionsFlagLeavesLocalToGeometryWhichRecoversIt) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter wrep;
    // Local FIRST so the demotion is VISIBLE: a demoted local that trails a
    // surviving atom is covered by it and nothing can tell (see
    // `threeShapeModule`).
    AssembledModule mod = threeShapeModule(/*localFirst=*/true);
    auto obj = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_EQ(rd32(obj, kHdrFlagsOff) & kMhSubsections, kMhSubsections);

    // Clear MH_SUBSECTIONS_VIA_SYMBOLS -- an object from a producer that makes
    // no such claim.
    obj[kHdrFlagsOff]     = 0;
    obj[kHdrFlagsOff + 1] = 0;
    obj[kHdrFlagsOff + 2] = 0;
    obj[kHdrFlagsOff + 3] = 0;

    DiagnosticReporter rrep;
    auto got = macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value())
        << "with no subsection declaration the local is not undecidable after "
           "all -- no atom covers offset 0, so it starts a body (errors="
        << rrep.errorCount() << ")";
    ASSERT_EQ(rrep.errorCount(), 0u);

    ASSERT_EQ(got->functions.size(), 2u)
        << "TWO atoms -- and the count is what discriminates, because the byte "
           "total is 12 whether the local was recovered, absorbed into the "
           "global, or split at its own block label";
    auto const* loc = funcOfSize(*got, 8u);
    ASSERT_NE(loc, nullptr)
        << "the local keeps its FULL 8 bytes -- stopping at the block label at "
           "+4 would be the split the run rule exists to prevent";
    EXPECT_EQ(nameOf(*got, loc->symbol), "_sym_11");
    auto const* pub = funcOfSize(*got, 4u);
    ASSERT_NE(pub, nullptr);
    EXPECT_EQ(nameOf(*got, pub->symbol), "_realfn");

    auto const* recovered = symNamed(*got, "_sym_11");
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->binding, SymbolBinding::Local)
        << "geometry says WHERE the bytes are, never who may see them";
    std::size_t named = 0;
    for (auto const& sy : got->symbols) named += (sy.name == "_sym_11") ? 1u : 0u;
    EXPECT_EQ(named, 1u)
        << "a promoted symbol already had its ModuleSymbol -- a second one is a "
           "duplicate definition to the cross-CU resolve";
}

// THE RUN RULE, Mach-O's branch of it -- and the reason the test above does not
// already cover it.
//
// There are TWO independent mechanisms that keep the block label at +4 from
// splitting the local body, and the test above only exercises the first:
//   1. ELIGIBILITY -- the writer stamps N_ALT_ENTRY on block labels, and the
//      fallback never touches a symbol the producer declared anything about.
//   2. THE RUN RULE -- two uncovered symbols with no reconstructed atom between
//      them are undecidable (two bodies, or one body plus a label), so only the
//      run's FIRST is promoted and the rest end up inside it.
// Clearing the N_ALT_ENTRY bit as well as the header flag removes (1) and leaves
// the object relying on (2) alone. The answer must not change: a body split at
// an interior label is a miscompile whichever mechanism was supposed to prevent
// it, and 8 + 4 sums to 12 either way, so the ATOM COUNT is what discriminates.
//
// RED-ON-DISABLE (watched): delete the run check in
// `uncoveredDefinedSymbolsThatStartAnAtom` -- the label at +4 is promoted too,
// the local splits into 4 + 4, and the count goes 2 -> 3.
TEST(MachoObjectReader, UndeclaredInteriorLabelIsHeldInsideItsBodyByTheRunRule) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter wrep;
    AssembledModule mod = threeShapeModule(/*localFirst=*/true);
    auto obj = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);

    // (1) No header declaration.
    obj[kHdrFlagsOff]     = 0;
    obj[kHdrFlagsOff + 1] = 0;
    obj[kHdrFlagsOff + 2] = 0;
    obj[kHdrFlagsOff + 3] = 0;

    // (2) ...and no per-symbol declaration either: clear the block label's
    //     N_ALT_ENTRY. Re-measured from the bytes first, so this cannot become a
    //     no-op if the writer ever stops stamping it.
    SymtabLoc const st = symtabOf(obj);
    std::size_t const blkOff = nlistOffsetOfName(obj, st, "_sym_77");
    ASSERT_NE(blkOff, static_cast<std::size_t>(-1))
        << "the synthetic block label must be in the symbol table for this test "
           "to be testing anything";
    std::uint16_t const blkDesc =
        static_cast<std::uint16_t>(obj[blkOff + kNlistDescOff])
        | static_cast<std::uint16_t>(
              static_cast<std::uint16_t>(obj[blkOff + kNlistDescOff + 1]) << 8);
    ASSERT_EQ(blkDesc & kNAltEntry, kNAltEntry)
        << "the writer must be stamping N_ALT_ENTRY -- otherwise mechanism (1) "
           "was never in play and this test proves nothing new";
    obj[blkOff + kNlistDescOff]     = 0;
    obj[blkOff + kNlistDescOff + 1] = 0;

    DiagnosticReporter rrep;
    auto got = macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value()) << "errors=" << rrep.errorCount();
    ASSERT_EQ(rrep.errorCount(), 0u);
    ASSERT_EQ(got->functions.size(), 2u)
        << "the run rule alone must hold the interior label inside the body -- "
           "promoting it would split a function at an interior label, which the "
           "linker may then lay out with padding between the halves";
    auto const* loc = funcOfSize(*got, 8u);
    ASSERT_NE(loc, nullptr)
        << "the local keeps all 8 bytes; stopping at 4 is the split";
    EXPECT_EQ(nameOf(*got, loc->symbol), "_sym_11");
    EXPECT_EQ(funcOfSize(*got, 4u) != nullptr, true)
        << "and the global is still its own 4-byte atom";
}

// ★★★ AND THE REFUSAL IS STILL LIVE, on the one shape geometry is forbidden to
// touch: a symbol the producer DECLARED to be an alternate entry, sitting where
// there is no atom for it to be an alternate entry INTO.
//
// That is a producer contradicting itself, and it is the reason the fallback is
// gated on `!altEntry` rather than simply run over every demoted symbol. An
// inference that "corrected" this would convert a self-inconsistent object into
// a silent, plausible-looking reconstruction -- exactly the class of repair this
// reader must never make.
TEST(MachoObjectReader, AltEntryWithNoAtomBeforeItIsRefusedNotRecovered) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter wrep;
    // Local FIRST: nothing precedes it, so nothing can absorb it.
    AssembledModule mod = threeShapeModule(/*localFirst=*/true);
    auto obj = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);

    SymtabLoc const st = symtabOf(obj);
    std::size_t const locOff = nlistOffsetOfName(obj, st, "_sym_11");
    ASSERT_NE(locOff, static_cast<std::size_t>(-1));
    ASSERT_EQ(obj[locOff + kNlistDescOff], 0u);
    ASSERT_EQ(obj[locOff + kNlistDescOff + 1], 0u);
    obj[locOff + kNlistDescOff]     = static_cast<std::uint8_t>(kNAltEntry & 0xFF);
    obj[locOff + kNlistDescOff + 1] = static_cast<std::uint8_t>(kNAltEntry >> 8);

    DiagnosticReporter rrep;
    auto got = macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rrep);
    EXPECT_FALSE(got.has_value())
        << "the object says this symbol is interior to something, and it is "
           "interior to nothing -- that must stay loud";
    bool namedTheSymbol = false;
    for (auto const& d : rrep.all()) {
        if (d.code == DiagnosticCode::F_ObjectReaderSymbolBodyDropped
            && d.actual.find("_sym_11") != std::string::npos) {
            namedTheSymbol = true;
        }
    }
    EXPECT_TRUE(namedTheSymbol)
        << "the refusal must be the coverage post-condition AND must name the "
           "symbol the object contradicted itself about";
}

// -- 7. A REAL clang-produced object: the same pair, from a foreign
//       producer ---------------------------------------------------
//
// The tests above run DSS's writer against DSS's reader, which proves the pair
// is applied CONSISTENTLY but cannot prove it is the FORMAT's vocabulary rather
// than a DSS convention. These bytes settle that: upstream clang 19, cross
// -emitting for arm64-apple-macos, sets the header flag and marks an
// `.alt_entry` symbol with n_desc 0x0200 -- and leaves a plain file-local
// `static` function's n_desc at 0. See the fixture header for full provenance
// and the byte-level contents.
//
// It is also the shape the anchor exists for: a `static` helper called from an
// exported function in an archive member.
//
// RED-ON-DISABLE (watched): with `startsAtom = isExt`, `_helper` is demoted and
// the coverage guard refuses the whole object -- ASSERT_TRUE goes red.

TEST(MachoObjectReader, ClangSubsectionsObjectClassifiesLocalsByAltEntry) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    auto obj = dss::test::clangMachoSubsectionsObject();
    // Re-measure the premise from the bytes rather than trusting the comment.
    ASSERT_EQ(rd32(obj, kHdrFlagsOff) & kMhSubsections, kMhSubsections)
        << "the fixture must be a subsections-via-symbols object";

    DiagnosticReporter rrep;
    auto got = macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value())
        << "a clang `.o` with a file-local function must read back "
           "(errors=" << rrep.errorCount() << ")";
    ASSERT_EQ(rrep.errorCount(), 0u);

    // `_helper` -- non-external, n_desc = 0 -> ITS OWN ATOM, with its bytes.
    auto const* helper = funcNamed(*got, "_helper");
    ASSERT_NE(helper, nullptr)
        << "clang's file-local `_helper` must be reconstructed as an atom";
    EXPECT_EQ(helper->bytes.size(), 8u)
        << "_helper spans [0x0C, 0x14) of a 20-byte __text";
    auto const* helperSym = symNamed(*got, "_helper");
    ASSERT_NE(helperSym, nullptr);
    EXPECT_EQ(helperSym->binding, SymbolBinding::Local)
        << "a foreign `static` must not be published cross-CU either";

    // `_outer_alt` -- non-external, n_desc = N_ALT_ENTRY -> NOT an atom. It is
    // the discriminating twin: identical n_type and n_sect to `_helper`, and
    // nlist_64 has no size field, so n_desc is the only thing telling them
    // apart.
    ASSERT_NE(symNamed(*got, "_outer_alt"), nullptr)
        << "the alternate entry must survive as an identity";
    for (auto const& f : got->functions) {
        EXPECT_NE(nameOf(*got, f.symbol), "_outer_alt")
            << "an N_ALT_ENTRY symbol must never become an atom";
    }
    // `_outer` runs [0x00, 0x08) -- it absorbs the alternate entry at 0x04
    // instead of being cut in half by it.
    auto const* outer = funcNamed(*got, "_outer");
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->bytes.size(), 8u)
        << "_outer must absorb its alternate entry's bytes";

    // The exported caller keeps its relocation, and it targets the promoted
    // file-local atom BY NAME -- i.e. the classification actually reconnects
    // the call that the old rule left dangling.
    auto const* entry = funcNamed(*got, "_entry");
    ASSERT_NE(entry, nullptr);
    ASSERT_EQ(entry->relocations.size(), 1u);
    EXPECT_EQ(nameOf(*got, entry->relocations[0].target), "_helper")
        << "the BRANCH26 in `_entry` must resolve to the file-local atom";

    EXPECT_TRUE(got->externImports.empty());
    EXPECT_TRUE(got->dataItems.empty());

    // THREE atoms, and the count is pinned BY NAME rather than by a byte total:
    // clang emits a section-start label (`ltmp0`) at offset 0, the SAME offset
    // as `_outer`, and the two are one body under two names. Reconstructing a
    // twin atom for `ltmp0` keeps every byte and keeps the sum right, so only
    // the identity set can tell the two reconstructions apart
    // (D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS).
    ASSERT_EQ(got->functions.size(), 3u)
        << "_outer, _entry, _helper -- clang's ltmp0 is another NAME for "
           "_outer's body, not a fourth function";
    std::vector<std::string> atomNames;
    for (auto const& f : got->functions) atomNames.push_back(nameOf(*got, f.symbol));
    std::sort(atomNames.begin(), atomNames.end());
    EXPECT_EQ(atomNames, (std::vector<std::string>{"_entry", "_helper", "_outer"}));

    // ...and `ltmp0` is not erased, it is REBOUND: same name, the owner's id.
    auto const* ltmp0 = symNamed(*got, "ltmp0");
    ASSERT_NE(ltmp0, nullptr) << "the alias must keep its identity";
    EXPECT_EQ(ltmp0->symbol, outer->symbol)
        << "one atom, several names -- the label resolves to the body it labels";
    EXPECT_EQ(nameOf(*got, outer->symbol), "_outer")
        << "the EXTERNAL name owns the atom, not the local label that shares "
           "its offset -- an id -> row lookup keeps the FIRST row, so the "
           "canonical one must be recorded first";
}

// ============================================================================
// -- 8. EQUAL-OFFSET DEFINED SYMBOLS ARE ONE ATOM UNDER SEVERAL NAMES -------
//
// D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS. Two defined symbols at
// one section offset are two NAMES for one body. Minting an atom per symbol
// produced byte-identical TWINS, and `findInterval` hands a relocation in the
// span they share to exactly ONE of them -- so the other shipped with its
// branch never patched. Not reachable from DSS's own output (its atoms all
// start at distinct offsets) and routine in clang's.
// ============================================================================

namespace {
// n_value's offset within the 16-byte nlist_64 (`kNlistSize` above).
constexpr std::size_t   kNlistValueOff = 8;
constexpr std::uint32_t kLcSymtab      = 0x02u;

// (symoff, nsyms) from LC_SYMTAB, or (0,0) when absent. WALKS the load-command
// chain rather than reading `symtabOf`'s fixed `kSymtabCmdOff`: that constant is
// derived from DSS's own single-section layout, and the clang fixtures below
// carry a different one.
[[nodiscard]] std::pair<std::size_t, std::uint32_t>
symtabSpan(std::vector<std::uint8_t> const& b) {
    std::uint32_t const ncmds = rd32(b, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd = rd32(b, off);
        std::uint32_t const sz  = rd32(b, off + 4);
        if (cmd == kLcSymtab) return {rd32(b, off + 8), rd32(b, off + 12)};
        if (sz == 0) break;
        off += sz;
    }
    return {0, 0};
}

// The index of the section-backed nlist whose n_value is `value`, or nsyms if
// none. Located BY VALUE rather than by a hard-coded slot, so the pin cannot
// quietly start pointing at a different symbol when the writer's symtab order
// changes.
[[nodiscard]] std::uint32_t nlistAtValue(std::vector<std::uint8_t> const& b,
                                         std::uint64_t value) {
    auto const span = symtabSpan(b);
    for (std::uint32_t i = 0; i < span.second; ++i) {
        std::size_t const e = span.first + static_cast<std::size_t>(i) * kNlistSize;
        if ((b[e + 4] & 0x0Eu) != 0x0Eu) continue;   // N_SECT (defined in a section)
        std::uint64_t v = 0;
        for (std::size_t k = 0; k < 8; ++k)
            v |= static_cast<std::uint64_t>(b[e + kNlistValueOff + k]) << (8u * k);
        if (v == value) return i;
    }
    return span.second;
}

void setNlistValue(std::vector<std::uint8_t>& b, std::uint32_t idx,
                   std::uint64_t value) {
    auto const span = symtabSpan(b);
    std::size_t const e = span.first + static_cast<std::size_t>(idx) * kNlistSize;
    for (std::size_t k = 0; k < 8; ++k)
        b[e + kNlistValueOff + k] =
            static_cast<std::uint8_t>((value >> (8u * k)) & 0xFFu);
}
} // namespace

// THE DEFECT, on REAL clang bytes: a section-start label at the first
// function's offset, and the function's own relocation INSIDE the span they
// share.
//
// RED-ON-DISABLE (watched): make `resolveEqualOffsetAtomAliases` skip its
// grouping arm (`if (h - g > 1)` -> `if (false)`) and `ltmp0` mints a twin
// again -- the count goes to 3, and the BRANCH26 at 0x08 is routed to the twin,
// so `_outer` comes back with ZERO relocations.
TEST(MachoObjectReader, ClangSectionStartLabelSharesTheFirstFunctionsAtom) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    auto obj = dss::test::clangMachoEqualOffsetLabelObject();
    // Re-measure the premise from the bytes rather than trusting the comment:
    // this object must really declare subsections-via-symbols, and the label at
    // offset 0 must really come FIRST in the symbol table.
    ASSERT_EQ(rd32(obj, kHdrFlagsOff) & kMhSubsections, kMhSubsections);
    ASSERT_EQ(nlistAtValue(obj, 0u), 0u)
        << "the first section-backed nlist must be the one at offset 0 "
           "(clang's ltmp0) -- the local label sorts AHEAD of _outer, which is "
           "why the EXTERNAL name is the one that used to lose the routing";

    DiagnosticReporter rrep;
    auto got = macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value())
        << "a plain clang `.o` must read back (errors=" << rrep.errorCount() << ")";
    ASSERT_EQ(rrep.errorCount(), 0u);

    // TWO atoms, pinned BY NAME. The byte total is identical whether `ltmp0`
    // twinned `_outer` or not, so a sum-based assertion would pass under the
    // exact defect this test exists for.
    ASSERT_EQ(got->functions.size(), 2u)
        << "_outer and _entry -- ltmp0 is another NAME for _outer's body";
    std::vector<std::string> atomNames;
    for (auto const& f : got->functions) atomNames.push_back(nameOf(*got, f.symbol));
    std::sort(atomNames.begin(), atomNames.end());
    EXPECT_EQ(atomNames, (std::vector<std::string>{"_entry", "_outer"}));

    auto const* outer = funcNamed(*got, "_outer");
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->bytes.size(), 0x18u) << "_outer runs [0x00, 0x18)";

    // THE CORRECTNESS PROPERTY. The BRANCH26 at section offset 0x08 lies inside
    // [0x00, 0x18) -- the span `ltmp0` and `_outer` share -- and it must reach
    // `_outer`, at item-relative offset 8. Under the twin defect it reached
    // `ltmp0`'s copy instead and `_outer` carried none, which ships an
    // un-patched `bl` (its immediate is still 0 in these bytes: a branch to
    // itself).
    ASSERT_EQ(outer->relocations.size(), 1u)
        << "_outer's own call must be attached to _outer";
    EXPECT_EQ(outer->relocations[0].offset, 8u);
    EXPECT_EQ(nameOf(*got, outer->relocations[0].target), "_sink");

    auto const* entry = funcNamed(*got, "_entry");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->bytes.size(), 0x18u);
    ASSERT_EQ(entry->relocations.size(), 1u);
    EXPECT_EQ(entry->relocations[0].offset, 8u)
        << "section offset 0x20 made relative to _entry at 0x18";

    // The label keeps its name and takes the body's identity.
    auto const* ltmp0 = symNamed(*got, "ltmp0");
    ASSERT_NE(ltmp0, nullptr);
    EXPECT_EQ(ltmp0->symbol, outer->symbol);
    EXPECT_EQ(nameOf(*got, outer->symbol), "_outer")
        << "the EXTERNAL name owns the atom even though the LOCAL label comes "
           "first in the symbol table";
}

// THE TARGET HALF: a relocation that NAMES an alias binds to the atom that owns
// the body. Without it the collapse would trade a silent miscompile for a
// spurious `K_SymbolUndefined` -- the linker's compound index declares only ids
// that own a body.
//
// DSS's own writer cannot emit two symbols at one offset, so the shape is made
// by moving ONE nlist n_value in the writer's own output; every other byte is
// the writer's. RED-ON-DISABLE (watched): `rel.target = SymbolId{ownerOf(...)}`
// -> `SymbolId{rSymNum}` and the reloc comes back naming `fn2`, an identity no
// atom owns.
TEST(MachoObjectReader, RelocationTargetingAnEqualOffsetAliasBindsToTheOwner) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction fn1;
    fn1.symbol = SymbolId{1};
    fn1.bytes.assign(16, 0x1F);
    // A BRANCH26 at offset 8 whose target is fn2 -- the symbol that becomes an
    // alias below.
    fn1.relocations.push_back(Relocation{8u, SymbolId{2}, RelocationKind{1}, 0});
    AssembledFunction fn2;
    fn2.symbol = SymbolId{2};
    fn2.bytes.assign(16, 0x2F);
    mod.functions = {fn1, fn2};
    mod.symbols = {
        ModuleSymbol{SymbolId{1}, "fn1", SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{2}, "fn2", SymbolBinding::Global, SymbolVisibility::Default},
    };

    DiagnosticReporter wrep;
    auto obj = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);

    // Measure the premise before mutating it: fn1 sits at 0, fn2 at 16, and
    // fn1's nlist comes FIRST -- so after the move fn1 is the lower-indexed of
    // two equally-visible names and owns the atom.
    std::uint32_t const idx1 = nlistAtValue(obj, 0u);
    std::uint32_t const idx2 = nlistAtValue(obj, 16u);
    std::uint32_t const nsyms = symtabSpan(obj).second;
    ASSERT_LT(idx1, nsyms);
    ASSERT_LT(idx2, nsyms);
    ASSERT_LT(idx1, idx2);
    setNlistValue(obj, idx2, 0u);   // fn2 now starts where fn1 does

    DiagnosticReporter rrep;
    auto got = macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rrep);
    ASSERT_TRUE(got.has_value()) << "errors=" << rrep.errorCount();
    ASSERT_EQ(rrep.errorCount(), 0u);

    ASSERT_EQ(got->functions.size(), 1u)
        << "fn1 and fn2 name one body now -- two atoms would be twins";
    AssembledFunction const& atom = got->functions[0];
    EXPECT_EQ(nameOf(*got, atom.symbol), "fn1");
    auto const* alias = symNamed(*got, "fn2");
    ASSERT_NE(alias, nullptr) << "the alias must keep its identity";
    EXPECT_EQ(alias->symbol, atom.symbol);

    ASSERT_EQ(atom.relocations.size(), 1u);
    EXPECT_EQ(atom.relocations[0].offset, 8u);
    EXPECT_EQ(atom.relocations[0].target, atom.symbol)
        << "a target naming the alias must bind to the atom that owns the body";
    EXPECT_EQ(nameOf(*got, atom.relocations[0].target), "fn1");
}

// ── THE MACH-O LEG OF THE THREE-READER ALIGNMENT SWEEP ────────────────────
//    D-FORMAT-MACHO-SECTION-ALIGN-EMITTED-RAW-NOT-LOG2 (the read-side half)
//
// The parent row is about `section_64.align` being written RAW where the field
// is a LOG2 exponent, and its closing work says to check the other walkers for
// the same class of mistake. Running that sweep across the READ direction found
// the COFF reader dropping its equivalent field entirely -- and found that
// NEITHER surviving reader had ever been pinned for carrying its own.
// ✔MEASURED 2026-08-27: a grep for an EXPECT/ASSERT on `alignment` across
// `tests/link/**` returned nothing at all.
//
// ★ THIS IS THE LEG WHERE THE ENCODING TRAP IS SHARPEST, because this reader
// faces the SAME field the row was filed about, from the other side. Reading
// `section_64.align` as a raw byte count answers 5 where 32 is right -- which
// is not even a power of two, so it degrades to 1 and the atom silently loses
// its constraint. That is the read-side mirror of writing 16 into a field that
// then claims 2^16.
//
// RED ON DISABLE: drop `di.alignment = alignFromLog2(sec.align)` in
// `macho_object_reader.cpp`, or route it through
// `foreignSectionAlignmentFromByteCount` instead of `...FromLog2`.
TEST(MachOObjectReader, DeclaredSectionAlignmentSurvivesTheRoundTrip) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    // One item per section, each with a DIFFERENT alignment, so a reader
    // answering a single constant cannot pass. `section_64.align` is
    // section-granular and the writer raises it to the section's strictest
    // member, so one member per section makes the item's own alignment the
    // value that must come back.
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};   // ret
    mod.functions.push_back(fn);

    AssembledData wide;
    wide.symbol    = SymbolId{10};
    wide.section   = DataSectionKind::Rodata;
    wide.bytes.assign(32, 0xAB);
    wide.alignment = Alignment::of<32>();
    mod.dataItems.push_back(wide);

    AssembledData narrow;
    narrow.symbol    = SymbolId{11};
    narrow.section   = DataSectionKind::Data;
    narrow.bytes     = {7, 0, 0, 0};
    narrow.alignment = Alignment::of<4>();
    mod.dataItems.push_back(narrow);

    mod.symbols = {
        ModuleSymbol{SymbolId{1},  "_fn",     SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{10}, "_wide",   SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{11}, "_narrow", SymbolBinding::Global, SymbolVisibility::Default},
    };

    DiagnosticReporter wrep;
    auto objBytes = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_FALSE(objBytes.empty());

    DiagnosticReporter rrep;
    auto readOpt = macho::readRelocatableObject(objBytes, *loaded.target,
                                                *loaded.format, rrep);
    ASSERT_TRUE(readOpt.has_value());
    ASSERT_EQ(rrep.errorCount(), 0u);

    auto const* rWide   = dataNamed(*readOpt, "_wide");
    auto const* rNarrow = dataNamed(*readOpt, "_narrow");
    ASSERT_NE(rWide, nullptr);
    ASSERT_NE(rNarrow, nullptr);
    EXPECT_EQ(rWide->alignment.bytes(), 32u)
        << "section_64.align is a LOG2 EXPONENT; the emitted 5 must come back "
           "as 32 bytes, never as the raw 5";

    // ⚠ THE FIRST DRAFT OF THIS TEST ASSERTED 4 HERE AND WENT RED AT 8, exactly
    // as its ELF twin did -- the read-back is SECTION-granular and the shipped
    // `__data` row declares an `addrAlign` FLOOR the walker H1-RAISES but never
    // goes below. The recovered value is `max(floor, strictest member)`:
    // `_wide` raises its section above the floor, `_narrow` sits under it.
    // Reading the floor from the schema rather than writing the literal 8 keeps
    // the pin honest across a schema edit.
    auto const* dataRow = loaded.format->sectionByKind(SectionKind::Data);
    ASSERT_NE(dataRow, nullptr);
    EXPECT_EQ(rNarrow->alignment.bytes(),
              std::max<std::uint64_t>(dataRow->addrAlign, 4u))
        << "the recovered alignment is the SECTION's -- the schema floor raised "
           "to the strictest member, never the member's own value on its own";
    EXPECT_NE(rWide->alignment.bytes(), rNarrow->alignment.bytes())
        << "a reader answering one constant for every section would satisfy "
           "either assertion above on its own";
}
