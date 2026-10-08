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
#include "link/linker.hpp"   // the image half of the call-frame pins
#include "link/object_format_schema.hpp"

#include "clang_macho_equal_offset_label_object.inc"
#include "clang_macho_subsections_object.inc"
#include "format_reject_support.hpp"   // countAtPath / countWithMessage / rejectSummary
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
#include <span>
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

    // -- extern imports: names + the kind each states (a call -> a function; an
    //    address states nothing, so its definition decides, P69
    //    D-LK-MEMBER-UNTYPED-EXTERN-TAKEN-AS-DATA) --
    auto const* ePuts = externNamed(got, "puts");
    auto const* eEnv = externNamed(got, "env");
    ASSERT_NE(ePuts, nullptr);
    ASSERT_NE(eEnv, nullptr);
    EXPECT_FALSE(ePuts->isData)
        << "puts is reached via a BRANCH26 call -> a FUNCTION import";
    EXPECT_EQ(ePuts->kindOrigin, ExternKindOrigin::Stated) << "a call states the kind";
    EXPECT_EQ(eEnv->kindOrigin, ExternKindOrigin::Pending)
        << "env is reached via a PAGE21 address reloc, which names a function and a datum alike -> the "
           "object states no kind, and the definition the link finds decides it";

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
    // reader that forced every reached extern to a function (a Stated row)
    // could not pass the second. The seed is not the answer in either
    // direction: the wire carries no kind but the call's.
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
    EXPECT_EQ(ePuts->kindOrigin, ExternKindOrigin::Stated);
    EXPECT_EQ(eEnv->kindOrigin, ExternKindOrigin::Pending)
        << "reached by X86_64_RELOC_UNSIGNED_8, which declares no call role "
           "-> the object states NO kind, and the definition decides it (P69, "
           "D-LK-MEMBER-UNTYPED-EXTERN-TAKEN-AS-DATA). Without this half, "
           "'every reached extern is a function' would also be green";
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
    EXPECT_EQ(ep->kindOrigin, ExternKindOrigin::Stated)
        << "and the call is what states the kind (a row the alias displaced "
           "would read back Pending)";
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

// ============ MachoUnwindRelocations =================================
// THE DWARF CALL-FRAME SECTION WHOSE REFERENCES ARE RELOCATION PAIRS (P69)
//   D-LK-MACHO-LD-R-EH-FRAME-RELOCATIONS-REFUSED-AT-READ
//   D-LK-MACHO-ARM64-EH-FRAME-SECTION-REFUSED-AT-READ
//
// A compiler's x86_64 Mach-O object stores each reference of `__TEXT,__eh_frame`
// RESOLVED and carries no relocation there. Two other producers do not:
//   * a relocatable link (`ld -r`), on BOTH ISAs, keeps every such reference as
//     a PAIR of relocations -- the symbol to subtract, then at the same address
//     the symbol it is subtracted from -- and stores only the addend;
//   * an arm64 COMPILER's own object, where it carries the section at all,
//     already keeps each record's function address as one such pair.
// The reader refused the first by name and the second as "corrupted" (the arm64
// documents had no row for the section); it now classifies the section on both
// ISAs and applies the pairs its format document declares
// (`macho.differenceRelocations`) on a copy of the section before it decodes.
//
// THE FIXTURES are Apple's own bytes: `tests/link/data/relinked_unwind.source.c`
// carries their source, the commands and the toolchain. The object AS COMPILED
// is the CONTROL of every pin -- the relinked object must read EXACTLY as it
// does -- so these pins run on every host; what DSS's programs of such objects
// exit with is measured where Apple's tools run (`RecordSymbolIdsNative`, and
// the example `examples/c/macho_relinked_object_call_frames`).
// =====================================================================

namespace {

using dss::link_format::test::countAtPath;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;

[[nodiscard]] std::vector<std::uint8_t> unwindFixture(char const* name) {
    auto const path = dss::test::repoRoot() / "tests" / "link" / "data" / name;
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    std::string const raw{std::istreambuf_iterator<char>{in},
                          std::istreambuf_iterator<char>{}};
    return std::vector<std::uint8_t>(raw.begin(), raw.end());
}

[[nodiscard]] std::string shippedFormatText(char const* stem) {
    auto const root = dss::test::findConfigRoot();
    if (!root.has_value()) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    std::ifstream in{*root / "object-formats" / (std::string{stem} + ".format.json"),
                     std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void wr32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
    for (std::size_t k = 0; k < 4; ++k) {
        b[o + k] = static_cast<std::uint8_t>(v >> (8u * k));
    }
}

// Where `__TEXT,__eh_frame`'s record, bytes and relocation table sit in an
// MH_OBJECT, and where its symbol table is -- found by walking the load
// commands, since these are a reference toolchain's layouts, not DSS's.
struct UnwindLayout {
    bool          found      = false;
    std::size_t   recordOff  = 0;   // the section_64 record
    std::size_t   bodyOff    = 0;
    std::size_t   bodySize   = 0;
    std::size_t   relocOff   = 0;
    std::uint32_t relocCount = 0;
    SymtabLoc     symtab{0, 0, 0};
};

[[nodiscard]] UnwindLayout unwindLayoutOf(std::vector<std::uint8_t> const& obj) {
    UnwindLayout out;
    if (obj.size() < 32) return out;
    auto fixedName = [&](std::size_t at) {
        std::string n;
        for (std::size_t k = 0; k < 16 && obj[at + k] != 0; ++k) {
            n.push_back(static_cast<char>(obj[at + k]));
        }
        return n;
    };
    std::uint32_t const ncmds = rd32(obj, 16);
    std::size_t         cmd   = 32;
    for (std::uint32_t i = 0; i < ncmds && cmd + 8 <= obj.size(); ++i) {
        std::uint32_t const kind = rd32(obj, cmd);
        std::uint32_t const size = rd32(obj, cmd + 4);
        if (kind == 0x19u) {   // LC_SEGMENT_64: 72 bytes, then its section_64 records (80 each)
            std::uint32_t const nsects = rd32(obj, cmd + 64);
            for (std::uint32_t s = 0; s < nsects; ++s) {
                std::size_t const rec = cmd + 72 + static_cast<std::size_t>(s) * 80;
                if (fixedName(rec) != "__eh_frame" || fixedName(rec + 16) != "__TEXT") continue;
                out.found      = true;
                out.recordOff  = rec;
                out.bodySize   = rd32(obj, rec + 40);
                out.bodyOff    = rd32(obj, rec + 48);
                out.relocOff   = rd32(obj, rec + 56);
                out.relocCount = rd32(obj, rec + 60);
            }
        } else if (kind == 0x2u) {   // LC_SYMTAB
            out.symtab = SymtabLoc{rd32(obj, cmd + 8), rd32(obj, cmd + 12), rd32(obj, cmd + 16)};
        }
        if (size == 0) break;
        cmd += size;
    }
    return out;
}

[[nodiscard]] std::string saidBy(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) out += "\n  " + d.actual;
    return out;
}

[[nodiscard]] bool saidWithCode(DiagnosticReporter const& rep, DiagnosticCode code,
                                std::string_view fragment) {
    for (auto const& d : rep.all()) {
        if (d.code == code && d.actual.find(fragment) != std::string::npos) return true;
    }
    return false;
}

// One ISA's fixtures, the documents a reader is handed for them, and what was
// MEASURED at their build (Apple clang 21.0.0, ld-1267).
struct UnwindCase {
    char const*              target;
    char const*              compiled;
    char const*              relinked;
    std::vector<char const*> documents;
    // The document an IMAGE of the ISA is linked under, and the ISA's one
    // instruction that returns: the body of the unit that holds the image's
    // entry in the image pin (x86_64 `ret`; arm64 `ret`, little-endian).
    char const*              image;
    std::vector<std::uint8_t> returns;
    // The relocation count of `__TEXT,__eh_frame` in each object: the x86_64
    // compiler writes none; the arm64 compiler one pair per record; the linker
    // two pairs per record on both.
    std::uint32_t            compiledRelocations;
    std::uint32_t            relinkedRelocations;
    // The byte length each function's record covers, `_dss_unwind_first` then
    // `_dss_unwind_shared` -- different, so a record on the wrong function shows.
    std::uint32_t            extents[2];
    // The (subtrahend, minuend) wire ids the documents declare, as MEASURED in
    // these fixtures' relocation tables.
    std::vector<MachODifferenceRelocation> pairs;
};

[[nodiscard]] std::vector<UnwindCase> unwindCases() {
    return {
        UnwindCase{"x86_64", "compiled_unwind_x86_64_macho.o", "relinked_unwind_x86_64_macho.o",
                   {"macho64-x86_64-darwin", "macho64-x86_64-darwin-staticlib"},
                   "macho64-x86_64-darwin-exec", {0xC3}, 0u, 8u, {11u, 14u},
                   {MachODifferenceRelocation{1409286144u, 67108864u},
                    MachODifferenceRelocation{1442840576u, 100663296u}}},
        UnwindCase{"arm64", "compiled_unwind_arm64_macho.o", "relinked_unwind_arm64_macho.o",
                   {"macho64-arm64-darwin", "macho64-arm64-darwin-staticlib"},
                   "macho64-arm64-darwin-exec", {0xC0, 0x03, 0x5F, 0xD6}, 4u, 8u, {8u, 24u},
                   {MachODifferenceRelocation{369098752u, 100663296u},
                    MachODifferenceRelocation{335544320u, 67108864u}}},
    };
}

constexpr char const* kUnwindFunctions[] = {"_dss_unwind_first", "_dss_unwind_shared"};

[[nodiscard]] std::uint64_t rd64(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint64_t>(rd32(b, o))
         | (static_cast<std::uint64_t>(rd32(b, o + 4)) << 32);
}

// The call-frame records of a linked IMAGE: where each begins and how far it
// reaches. The image's own CIE states how a record's address fields are
// encoded; the two encodings a Mach-O image of these targets can carry are
// read, and any other is SAID rather than guessed at. nullopt: the image has no
// `__TEXT,__eh_frame`.
struct ImageFrame {
    std::uint64_t begin  = 0;
    std::uint64_t length = 0;
};

[[nodiscard]] std::optional<std::vector<ImageFrame>>
imageCallFrames(std::vector<std::uint8_t> const& image) {
    auto const at = unwindLayoutOf(image);
    if (!at.found) return std::nullopt;
    std::uint64_t const     sectionVa = rd64(image, at.recordOff + 32);
    std::size_t const       end       = at.bodyOff + at.bodySize;
    std::vector<ImageFrame> frames;
    std::uint8_t            encoding = 0xFFu;
    for (std::size_t p = at.bodyOff; p + 8 <= end;) {
        std::uint32_t const length = rd32(image, p);
        if (length == 0) break;
        std::size_t const body = p + 4;
        if (rd32(image, body) == 0) {   // a CIE
            std::size_t        q       = body + 4;
            std::uint8_t const version = image[q++];
            std::string        augmentation;
            while (image[q] != 0) augmentation.push_back(static_cast<char>(image[q++]));
            ++q;
            auto const skipLeb = [&] {
                while ((image[q++] & 0x80u) != 0) {}
            };
            skipLeb();   // code alignment
            skipLeb();   // data alignment
            if (version == 1) {
                ++q;     // the return-address column is one byte in version 1
            } else {
                skipLeb();
            }
            if (augmentation != "zR") {
                ADD_FAILURE() << "the image's CIE states augmentation '" << augmentation
                              << "', which this pin does not read";
                return frames;
            }
            skipLeb();   // the augmentation data's length
            encoding = image[q];
        } else {
            std::size_t const   field   = body + 4;
            std::uint64_t const fieldVa = sectionVa + (field - at.bodyOff);
            ImageFrame          frame;
            if (encoding == 0x1Bu) {          // pc-relative, signed, four bytes
                frame.begin  = fieldVa + static_cast<std::uint64_t>(static_cast<std::int64_t>(
                                             static_cast<std::int32_t>(rd32(image, field))));
                frame.length = rd32(image, field + 4);
            } else if (encoding == 0x10u) {   // pc-relative, eight bytes
                frame.begin  = fieldVa + rd64(image, field);
                frame.length = rd64(image, field + 8);
            } else {
                ADD_FAILURE() << "the image's CIE states pointer encoding " << static_cast<unsigned>(encoding)
                              << " (decimal), which this pin does not read";
                return frames;
            }
            frames.push_back(frame);
        }
        p = body + length;
    }
    return frames;
}

// `stem`'s shipped document with ONE change, loaded.
template <typename Mutate>
[[nodiscard]] std::shared_ptr<ObjectFormatSchema> shippedFormatWith(char const* stem, Mutate mutate) {
    auto const text = shippedFormatText(stem);
    if (text.empty()) return nullptr;
    nlohmann::json doc = nlohmann::json::parse(text);
    mutate(doc);
    auto loaded = ObjectFormatSchema::loadFromText(doc.dump(), "a-shipped-document-with-one-change");
    if (!loaded.has_value()) {
        ADD_FAILURE() << stem << " with one change must still load: " << rejectSummary(loaded);
        return nullptr;
    }
    return std::move(loaded).value();
}

} // namespace

// THE PIN. Apple's relocatable link of the two-function unit reads EXACTLY as
// the compiler's object of it does: the same two bodies, each with the
// call-frame record of ITS OWN function (the extents differ, so a record
// attached to the other function could not agree), and nothing left over --
// the local labels the producers wrote into the section (`EH_Frame1`, one
// `func.eh` per record) name no symbol of the module, and no function arrives
// undescribed.
TEST(MachoUnwindRelocations, ARelocatableLinksProductReadsAsTheCompiledObjectDoes) {
    for (auto const& c : unwindCases()) {
        auto const compiledBytes = unwindFixture(c.compiled);
        auto const relinkedBytes = unwindFixture(c.relinked);
        ASSERT_FALSE(compiledBytes.empty());
        ASSERT_FALSE(relinkedBytes.empty());
        // THE PREMISE, read off the fixtures: each has the relocation count
        // measured at its build.
        auto const compiledLayout = unwindLayoutOf(compiledBytes);
        auto const relinkedLayout = unwindLayoutOf(relinkedBytes);
        ASSERT_TRUE(compiledLayout.found) << c.compiled << " holds no `__TEXT,__eh_frame`";
        ASSERT_TRUE(relinkedLayout.found) << c.relinked << " holds no `__TEXT,__eh_frame`";
        ASSERT_EQ(compiledLayout.relocCount, c.compiledRelocations)
            << c.compiled << ": the fixture is not the shape this pin was measured on";
        ASSERT_EQ(relinkedLayout.relocCount, c.relinkedRelocations)
            << c.relinked << ": the fixture is not the shape this pin was measured on";
        for (char const* document : c.documents) {
            SCOPED_TRACE(std::string{c.target} + " / " + document);
            auto loaded = loadShipped(c.target, document);
            ASSERT_TRUE(loaded.target && loaded.format);
            DiagnosticReporter crep;
            auto const compiled = macho::readRelocatableObject(compiledBytes, *loaded.target,
                                                               *loaded.format, crep);
            ASSERT_TRUE(compiled.has_value()) << "CONTROL: the compiler's object reads:" << saidBy(crep);
            DiagnosticReporter rrep;
            auto const relinked = macho::readRelocatableObject(relinkedBytes, *loaded.target,
                                                               *loaded.format, rrep);
            ASSERT_TRUE(relinked.has_value())
                << "Apple's relocatable link of the object must read:" << saidBy(rrep);
            EXPECT_EQ(crep.errorCount(), 0u) << saidBy(crep);
            EXPECT_EQ(rrep.errorCount(), 0u) << saidBy(rrep);
            EXPECT_EQ(rrep.all().size(), crep.all().size())
                << "the product is read with no more said than the compiler's object is:" << saidBy(rrep);
            EXPECT_FALSE(saidWithCode(crep, DiagnosticCode::K_UnwindRuleUnrepresentable, ""))
                << "CONTROL: every function of the compiler's object arrives described:" << saidBy(crep);
            EXPECT_FALSE(saidWithCode(rrep, DiagnosticCode::K_UnwindRuleUnrepresentable, ""))
                << "every function arrives described:" << saidBy(rrep);

            ASSERT_EQ(relinked->functions.size(), compiled->functions.size());
            for (std::size_t k = 0; k < 2; ++k) {
                SCOPED_TRACE(kUnwindFunctions[k]);
                auto const* want = funcNamed(*compiled, kUnwindFunctions[k]);
                auto const* got  = funcNamed(*relinked, kUnwindFunctions[k]);
                ASSERT_NE(want, nullptr);
                ASSERT_NE(got, nullptr);
                EXPECT_EQ(got->bytes, want->bytes);
                ASSERT_TRUE(want->cfi.has_value()) << "CONTROL: the compiled object's function is described";
                ASSERT_TRUE(got->cfi.has_value())
                    << "the relinked object's function arrived with no call-frame record";
                EXPECT_EQ(want->cfi->codeLength, c.extents[k])
                    << "CONTROL: the record the compiler wrote for THIS function";
                EXPECT_EQ(got->cfi->codeLength, want->cfi->codeLength);
                EXPECT_EQ(got->cfi->initial, want->cfi->initial);
                ASSERT_EQ(got->cfi->ops.size(), want->cfi->ops.size());
                for (std::size_t o = 0; o < want->cfi->ops.size(); ++o) {
                    EXPECT_EQ(got->cfi->ops[o].pcOffset, want->cfi->ops[o].pcOffset) << "rule #" << o;
                }
            }
            ASSERT_NE(c.extents[0], c.extents[1])
                << "THE PREMISE: the two records have different extents, so one attached to the "
                   "other function could not have agreed with the control";
            for (auto const* module : {&*compiled, &*relinked}) {
                for (auto const& s : module->symbols) {
                    EXPECT_NE(s.name, "EH_Frame1") << "a label of the call-frame section was published";
                    EXPECT_NE(s.name, "func.eh") << "a label of the call-frame section was published";
                    EXPECT_NE(s.name, "ltmp2") << "a label of the call-frame section was published";
                }
                EXPECT_EQ(externNamed(*module, "EH_Frame1"), nullptr);
                EXPECT_EQ(externNamed(*module, "func.eh"), nullptr);
                EXPECT_EQ(externNamed(*module, "ltmp2"), nullptr);
            }
        }
    }
}

// AN arm64 COMPILER's OWN OBJECT THAT CARRIES THE SECTION
// (D-LK-MACHO-ARM64-EH-FRAME-SECTION-REFUSED-AT-READ). It is the section ROW
// that reads it -- the shipped document MINUS that one row refuses the object
// as it was refused before, naming the section -- and the PAIRS that bind its
// records: the same object under the document MINUS its pairs is refused naming
// the key, because on arm64 the compiler itself leaves each record's function
// address to a pair.
TEST(MachoUnwindRelocations, AnArm64CompilersOwnObjectCarryingTheSectionReads) {
    auto const bytes = unwindFixture("compiled_unwind_arm64_macho.o");
    ASSERT_FALSE(bytes.empty());
    auto const at = unwindLayoutOf(bytes);
    ASSERT_TRUE(at.found);
    ASSERT_NE(at.relocCount, 0u) << "THE PREMISE: the arm64 compiler's own object carries relocations there";
    for (char const* document : {"macho64-arm64-darwin", "macho64-arm64-darwin-staticlib"}) {
        SCOPED_TRACE(document);
        auto loaded = loadShipped("arm64", document);
        ASSERT_TRUE(loaded.target && loaded.format);
        DiagnosticReporter rep;
        auto const read = macho::readRelocatableObject(bytes, *loaded.target, *loaded.format, rep);
        ASSERT_TRUE(read.has_value()) << saidBy(rep);
        EXPECT_EQ(rep.errorCount(), 0u) << saidBy(rep);
        for (char const* name : kUnwindFunctions) {
            auto const* fn = funcNamed(*read, name);
            ASSERT_NE(fn, nullptr) << name;
            EXPECT_TRUE(fn->cfi.has_value()) << name << " arrived with no call-frame record";
        }

        // THE ROW is the fix: without it the section is one DSS cannot classify.
        auto const noRow = shippedFormatWith(document, [](nlohmann::json& d) {
            auto& sections = d.at("sections");
            for (auto it = sections.begin(); it != sections.end(); ++it) {
                if (it->value("name", std::string{}) == "__eh_frame") {
                    sections.erase(it);
                    return;
                }
            }
            ADD_FAILURE() << "THE PREMISE: the shipped document declares a `__eh_frame` row";
        });
        ASSERT_NE(noRow, nullptr);
        DiagnosticReporter rowRep;
        EXPECT_FALSE(macho::readRelocatableObject(bytes, *loaded.target, *noRow, rowRep).has_value())
            << "CONTROL: without its row the section's own label has nowhere to live";
        EXPECT_NE(saidBy(rowRep).find("'__TEXT,__eh_frame'"), std::string::npos) << saidBy(rowRep);

        // THE PAIRS bind the records: without them the stored value is half a reference.
        auto const noPairs = shippedFormatWith(
            document, [](nlohmann::json& d) { d.at("macho").erase("differenceRelocations"); });
        ASSERT_NE(noPairs, nullptr);
        DiagnosticReporter pairRep;
        EXPECT_FALSE(macho::readRelocatableObject(bytes, *loaded.target, *noPairs, pairRep).has_value());
        EXPECT_TRUE(saidWithCode(pairRep, DiagnosticCode::F_UnsupportedBinaryFormat,
                                 "`macho.differenceRelocations`"))
            << saidBy(pairRep);
    }
}

// THE IMAGE HALF: A FUNCTION THAT ARRIVES DESCRIBED REACHES THE IMAGE DESCRIBED
// (the Mach-O part of D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE).
// Reading a record is half of carrying it. A PROGRAM is linked from two units,
// as a real one is -- a unit of its own that holds the entry (one instruction,
// no record), then one of these objects: the compiler's own and the relocatable
// link's product, both ISAs. Two units take the MERGE, which copies every
// function into the one module the image writer is handed; a record dropped
// there would leave the readers' pins and the writer's all green. The image
// holds, for each of the object's two functions, ONE call-frame record that
// begins at the function's own address and reaches exactly as far as the
// OBJECT's record did, and no other record: neither the program's own entry
// function nor the entry the link adds has one. CONTROL: the same units with
// the object's records taken away link to an image with no such section, so the
// records found are the object's and not something the image writer makes for
// every function.
// ✔MEASURED 2026-10-08 on the images `dsscp` builds of the two examples (a DSS
// `main` beside each object): three records -- `_main`'s and the two below, at
// 8 and 24 bytes on arm64, 11 and 14 on x86_64 -- and the image of the
// relocatable link's product is byte-identical to the image of the compiler's
// object.
TEST(MachoUnwindRelocations, TheImageDescribesEachFunctionAsItsObjectDid) {
    for (auto const& c : unwindCases()) {
        auto member = loadShipped(c.target, c.documents[0]);
        auto exec   = loadShipped(c.target, c.image);
        ASSERT_TRUE(member.target && member.format && exec.format);
        for (char const* fixture : {c.compiled, c.relinked}) {
            SCOPED_TRACE(std::string{c.target} + " / " + fixture);
            auto const bytes = unwindFixture(fixture);
            ASSERT_FALSE(bytes.empty());
            DiagnosticReporter rrep;
            auto read = macho::readRelocatableObject(bytes, *member.target, *member.format, rrep,
                                                     CompilationUnitId{2});
            ASSERT_TRUE(read.has_value()) << saidBy(rrep);
            ASSERT_EQ(read->functions.size(), 2u);

            // The program's own unit: its entry, which describes no frame.
            AssembledModule own;
            own.cuId              = CompilationUnitId{1};
            own.expectedFuncCount = 1;
            AssembledFunction start;
            start.symbol = SymbolId{1};
            start.bytes  = c.returns;
            own.functions.push_back(std::move(start));
            own.symbols         = {ModuleSymbol{SymbolId{1}, "_dss_image_pin_entry", SymbolBinding::Global,
                                                SymbolVisibility::Default}};
            own.userEntrySymbol = SymbolId{1};

            std::vector<AssembledModule> units;
            units.push_back(own);
            units.push_back(*read);
            DiagnosticReporter lrep;
            auto const linked = linker::link(std::span<AssembledModule const>{units}, *member.target, *exec.format,
                                             lrep, ImageRequest{.artifactFileName = "unwind_image"});
            ASSERT_TRUE(linked.ok()) << saidBy(lrep);
            auto const frames = imageCallFrames(linked.bytes);
            ASSERT_TRUE(frames.has_value()) << "the image carries no `__TEXT,__eh_frame`";
            auto const at = unwindLayoutOf(linked.bytes);
            for (std::size_t k = 0; k < 2; ++k) {
                SCOPED_TRACE(kUnwindFunctions[k]);
                std::size_t const record = nlistOffsetOfName(linked.bytes, at.symtab, kUnwindFunctions[k]);
                ASSERT_NE(record, static_cast<std::size_t>(-1)) << "the image names the function";
                std::uint64_t const address   = rd64(linked.bytes, record + 8);
                std::size_t         described = 0;
                for (auto const& frame : *frames) {
                    if (frame.begin != address) continue;
                    ++described;
                    EXPECT_EQ(frame.length, c.extents[k])
                        << "the image's record reaches as far as the OBJECT's record did";
                }
                EXPECT_EQ(described, 1u) << "ONE record of the image begins at the function";
            }
            EXPECT_EQ(frames->size(), 2u)
                << "the image describes the object's two functions and nothing else: the program's own entry "
                   "function states no record and the entry the link adds has none";

            std::vector<AssembledModule> bareUnits;
            bareUnits.push_back(own);
            bareUnits.push_back(*read);
            for (auto& fn : bareUnits[1].functions) fn.cfi.reset();
            DiagnosticReporter brep;
            auto const bareImage = linker::link(std::span<AssembledModule const>{bareUnits}, *member.target,
                                                *exec.format, brep,
                                                ImageRequest{.artifactFileName = "unwind_image"});
            ASSERT_TRUE(bareImage.ok()) << saidBy(brep);
            EXPECT_FALSE(unwindLayoutOf(bareImage.bytes).found)
                << "CONTROL: with the records taken away the image has no call-frame section at all";
        }
    }
}

// EVERY OTHER SHAPE IS REFUSED BY NAME. Each cell changes ONE fact of the
// relinked fixture's relocation table (in memory) and reads it: an entry that
// starts no declared pair, a subtrahend followed by another type, a pair split
// across addresses, a section-number entry, a symbol the object does not
// define, a field that runs past the section, two pairs on one field, a table
// that ends on a subtrahend, a value its field cannot hold. The control is the
// first test: the same bytes, unchanged, read. Both ISAs (✔MEASURED off the
// fixtures): entries 0 and 1 are the first record's 4-byte pair, 2 and 3 its
// 8-byte pair.
TEST(MachoUnwindRelocations, EveryOtherRelocationShapeIsRefusedByName) {
    // A `relocation_info` is eight bytes: `r_address`, then a word holding the
    // symbol number (bits 0-23), pc-relative (24), length (25-26), extern (27)
    // and type (28-31).
    struct Cell {
        char const*                                    label;
        void (*mutate)(std::vector<std::uint8_t>&, UnwindLayout const&);
        DiagnosticCode                                 code;
        char const*                                    says;
    };
    Cell const kCells[] = {
        {"the first entry is the minuend's type, which starts no pair",
         [](std::vector<std::uint8_t>& b, UnwindLayout const& l) {
             // swap entries 0 and 1 (a subtrahend and its minuend)
             for (std::size_t k = 0; k < 8; ++k) std::swap(b[l.relocOff + k], b[l.relocOff + 8 + k]);
         },
         DiagnosticCode::F_UnsupportedBinaryFormat, "is not the subtrahend of a difference pair"},
        {"the entry after the subtrahend is another type",
         [](std::vector<std::uint8_t>& b, UnwindLayout const& l) {
             // r_type is the top four bits of the second word: UNSIGNED (0) -> 1
             wr32(b, l.relocOff + 12, rd32(b, l.relocOff + 12) ^ (1u << 28));
         },
         DiagnosticCode::F_CorruptedBinary, "where the format declares its minuend as wire type"},
        {"the minuend sits at another address",
         [](std::vector<std::uint8_t>& b, UnwindLayout const& l) {
             wr32(b, l.relocOff + 8, rd32(b, l.relocOff + 8) + 4u);
         },
         DiagnosticCode::F_CorruptedBinary, "at the same offset"},
        {"the pair's field runs past the section",
         [](std::vector<std::uint8_t>& b, UnwindLayout const& l) {
             // both entries of the first (4-byte) pair, two bytes before the end
             std::uint32_t const at = static_cast<std::uint32_t>(l.bodySize) - 2u;
             wr32(b, l.relocOff, at);
             wr32(b, l.relocOff + 8, at);
         },
         DiagnosticCode::F_CorruptedBinary, "runs past the section"},
        {"the subtrahend names a section, not a symbol",
         [](std::vector<std::uint8_t>& b, UnwindLayout const& l) {
             wr32(b, l.relocOff + 4, rd32(b, l.relocOff + 4) & ~(1u << 27));
         },
         DiagnosticCode::F_UnsupportedBinaryFormat, "names a SECTION rather than a symbol (r_extern=0)"},
        {"the subtrahend's symbol is not defined by this object",
         [](std::vector<std::uint8_t>& b, UnwindLayout const& l) {
             // the nlist_64 the first entry names: N_SECT (0x0e) -> N_UNDF, section 0
             std::uint32_t const sym = rd32(b, l.relocOff + 4) & 0x00FFFFFFu;
             std::size_t const   rec = l.symtab.symoff + static_cast<std::size_t>(sym) * kNlistSize;
             b[rec + kNlistTypeOff]     = static_cast<std::uint8_t>(b[rec + kNlistTypeOff] & ~0x0Eu);
             b[rec + kNlistTypeOff + 1] = 0;
         },
         DiagnosticCode::F_UnsupportedBinaryFormat, "which this object does not define in one of its sections"},
        {"two pairs patch one field",
         [](std::vector<std::uint8_t>& b, UnwindLayout const& l) {
             // the second pair (entries 2 and 3) moved onto the first pair's address
             std::uint32_t const first = rd32(b, l.relocOff);
             wr32(b, l.relocOff + 16, first);
             wr32(b, l.relocOff + 24, first);
         },
         DiagnosticCode::F_CorruptedBinary, "carries two difference pairs on the field"},
        {"the table ends on a subtrahend",
         [](std::vector<std::uint8_t>& b, UnwindLayout const& l) {
             wr32(b, l.recordOff + 60, l.relocCount - 1u);
         },
         DiagnosticCode::F_CorruptedBinary, "the minuend that must follow it is missing"},
        {"the difference does not fit its field",
         [](std::vector<std::uint8_t>& b, UnwindLayout const& l) {
             // the minuend's symbol of the first (4-byte) pair, moved 2^40 bytes up
             std::uint32_t const sym = rd32(b, l.relocOff + 12) & 0x00FFFFFFu;
             std::size_t const   rec = l.symtab.symoff + static_cast<std::size_t>(sym) * kNlistSize;
             b[rec + 8 + 5] = 1;   // n_value is the record's last eight bytes
         },
         DiagnosticCode::F_CorruptedBinary, "-byte field cannot hold"},
    };
    for (auto const& c : unwindCases()) {
        auto const pristine = unwindFixture(c.relinked);
        ASSERT_FALSE(pristine.empty());
        auto const at = unwindLayoutOf(pristine);
        ASSERT_TRUE(at.found);
        ASSERT_GE(at.relocCount, 4u);
        auto loaded = loadShipped(c.target, c.documents.front());
        ASSERT_TRUE(loaded.target && loaded.format);
        for (auto const& cell : kCells) {
            SCOPED_TRACE(std::string{c.target} + ": " + cell.label);
            auto bytes = pristine;
            cell.mutate(bytes, at);
            ASSERT_NE(bytes, pristine) << "the cell changed nothing";
            DiagnosticReporter rep;
            auto const read = macho::readRelocatableObject(bytes, *loaded.target, *loaded.format, rep);
            EXPECT_FALSE(read.has_value()) << "this shape must not be read as though the pair had been applied";
            EXPECT_TRUE(saidWithCode(rep, cell.code, cell.says)) << "said:" << saidBy(rep);
            EXPECT_TRUE(saidWithCode(rep, cell.code, "unwind section '__TEXT,__eh_frame'"))
                << "the refusal names the section:" << saidBy(rep);
        }
    }
}

// THE ADDEND A NARROW FIELD STORES IS SIGNED. The fixtures' four-byte fields
// all store +4, so nothing above tells a sign-extended addend from a
// zero-extended one. This states the SAME reference with a negative addend:
// the first record's own label is moved eight bytes up (in memory), its
// four-byte field stores -4 where it stored +4, and its eight-byte field 0
// where it stored -8 -- every difference comes out as before, so the object
// must read exactly as the unchanged one does. Read as an unsigned number the
// -4 is four gigabytes, and the pair is refused as one its field cannot hold.
TEST(MachoUnwindRelocations, ANegativeAddendInANarrowFieldIsAnAddend) {
    for (auto const& c : unwindCases()) {
        SCOPED_TRACE(c.target);
        auto const pristine = unwindFixture(c.relinked);
        ASSERT_FALSE(pristine.empty());
        auto const at = unwindLayoutOf(pristine);
        ASSERT_TRUE(at.found);
        ASSERT_GE(at.relocCount, 4u);
        std::uint32_t const narrowField = rd32(pristine, at.relocOff);        // entries 0 and 1
        std::uint32_t const wideField   = rd32(pristine, at.relocOff + 16);   // entries 2 and 3
        std::uint32_t const label       = rd32(pristine, at.relocOff + 12) & 0x00FFFFFFu;
        // THE PREMISE: the label the narrow pair is measured TO is the one the
        // wide pair is measured FROM, and the fields store +4 and -8.
        ASSERT_EQ(rd32(pristine, at.relocOff + 20) & 0x00FFFFFFu, label);
        ASSERT_EQ(rd32(pristine, at.bodyOff + narrowField), 4u);
        ASSERT_EQ(rd32(pristine, at.bodyOff + wideField), 0xFFFFFFF8u);
        ASSERT_EQ(rd32(pristine, at.bodyOff + wideField + 4), 0xFFFFFFFFu);

        auto bytes = pristine;
        std::size_t const rec = at.symtab.symoff + static_cast<std::size_t>(label) * kNlistSize;
        ASSERT_LT(bytes[rec + 8], 0xF8u) << "the label's address does not carry into its next byte";
        bytes[rec + 8] = static_cast<std::uint8_t>(bytes[rec + 8] + 8u);   // n_value += 8
        wr32(bytes, at.bodyOff + narrowField, 0xFFFFFFFCu);                 // +4 -> -4
        wr32(bytes, at.bodyOff + wideField, 0u);                            // -8 -> 0
        wr32(bytes, at.bodyOff + wideField + 4, 0u);

        auto loaded = loadShipped(c.target, c.documents.front());
        ASSERT_TRUE(loaded.target && loaded.format);
        DiagnosticReporter wantRep;
        auto const want = macho::readRelocatableObject(pristine, *loaded.target, *loaded.format, wantRep);
        ASSERT_TRUE(want.has_value()) << "CONTROL:" << saidBy(wantRep);
        DiagnosticReporter gotRep;
        auto const got = macho::readRelocatableObject(bytes, *loaded.target, *loaded.format, gotRep);
        ASSERT_TRUE(got.has_value())
            << "a negative addend in a four-byte field is an addend, not a number its field cannot hold:"
            << saidBy(gotRep);
        for (char const* name : kUnwindFunctions) {
            auto const* w = funcNamed(*want, name);
            auto const* g = funcNamed(*got, name);
            ASSERT_NE(w, nullptr);
            ASSERT_NE(g, nullptr);
            ASSERT_TRUE(w->cfi.has_value());
            ASSERT_TRUE(g->cfi.has_value()) << name;
            EXPECT_EQ(g->cfi->codeLength, w->cfi->codeLength) << name;
            EXPECT_EQ(g->cfi->ops.size(), w->cfi->ops.size()) << name;
        }
    }
}

// THE PAIRS ARE THE DOCUMENT'S. The shipped document MINUS exactly
// `macho.differenceRelocations` still loads -- the key is a fact about what a
// reader of the format applies, not a required one -- and under it the
// linker's product is refused naming the key. The compiler's object is the
// control on x86_64, where it carries no relocation there and reads as before;
// on arm64 it carries its own pairs and is refused with the product.
TEST(MachoUnwindRelocations, ADocumentThatDeclaresNoPairRefusesTheSectionNamingTheKey) {
    for (auto const& c : unwindCases()) {
        SCOPED_TRACE(c.target);
        auto const compiledBytes = unwindFixture(c.compiled);
        auto const relinkedBytes = unwindFixture(c.relinked);
        ASSERT_FALSE(compiledBytes.empty());
        ASSERT_FALSE(relinkedBytes.empty());
        auto loaded = loadShipped(c.target, c.documents.front());
        ASSERT_TRUE(loaded.target && loaded.format);
        auto const stripped = shippedFormatWith(c.documents.front(), [](nlohmann::json& d) {
            ASSERT_TRUE(d.at("macho").contains("differenceRelocations"))
                << "THE PREMISE: the shipped document declares its pairs";
            d.at("macho").erase("differenceRelocations");
        });
        ASSERT_NE(stripped, nullptr);

        DiagnosticReporter rep;
        auto const refused = macho::readRelocatableObject(relinkedBytes, *loaded.target, *stripped, rep);
        EXPECT_FALSE(refused.has_value())
            << "a format that declares no difference pair cannot complete the section's references";
        EXPECT_TRUE(saidWithCode(rep, DiagnosticCode::F_UnsupportedBinaryFormat, "`macho.differenceRelocations`"))
            << "the refusal names the key whose rows are the whole fix:" << saidBy(rep);

        DiagnosticReporter crep;
        auto const compiled = macho::readRelocatableObject(compiledBytes, *loaded.target, *stripped, crep);
        if (c.compiledRelocations == 0u) {
            EXPECT_TRUE(compiled.has_value())
                << "CONTROL: the compiler's object carries no relocation there and needs no pair:" << saidBy(crep);
        } else {
            EXPECT_FALSE(compiled.has_value())
                << "this ISA's compiler leaves a pair there too, so its own object needs the key as well";
        }
    }
}

// A PIN ON THE SHIPPED VALUES: each member document declares exactly the wire
// ids MEASURED in its ISA's fixtures, and no image document declares any (the
// loader refuses the key there; this reads that none tries).
TEST(MachoUnwindRelocations, TheShippedDocumentsDeclareTheMeasuredPairs) {
    for (auto const& c : unwindCases()) {
        for (char const* document : c.documents) {
            SCOPED_TRACE(document);
            auto const loaded = ObjectFormatSchema::loadShipped(document);
            ASSERT_TRUE(loaded.has_value());
            auto const& declared = (*loaded)->macho().differenceRelocations;
            ASSERT_EQ(declared.size(), c.pairs.size());
            for (std::size_t i = 0; i < c.pairs.size(); ++i) {
                EXPECT_EQ(declared[i].subtrahendNativeId, c.pairs[i].subtrahendNativeId) << "pair #" << i;
                EXPECT_EQ(declared[i].minuendNativeId, c.pairs[i].minuendNativeId) << "pair #" << i;
            }
        }
    }
    for (char const* image : {"macho64-x86_64-darwin-exec", "macho64-x86_64-darwin-dylib",
                              "macho64-arm64-darwin-exec", "macho64-arm64-darwin-dylib"}) {
        auto const loaded = ObjectFormatSchema::loadShipped(image);
        ASSERT_TRUE(loaded.has_value()) << image;
        EXPECT_TRUE((*loaded)->macho().differenceRelocations.empty()) << image;
    }
}

// THE LOADER HOLDS THE TABLE TO ITS RULES, each at its own JSON pointer: a
// subtrahend id a relocation row already claims, one subtrahend naming two
// minuends, a subtrahend that is another pair's minuend, two ids that state
// different widths, a pc-relative id, an id that sets a bit belonging to one
// entry, a width restated beside the ids (the ids carry it: not a key), and
// the table on a document no reader is handed.
TEST(MachoUnwindRelocations, TheLoaderHoldsThePairsToTheirRules) {
    struct Cell {
        char const* label;
        char const* stem;
        void (*mutate)(nlohmann::json&);
        char const* path;
        char const* says;
    };
    Cell const kCells[] = {
        {"a relocation row claims the subtrahend's wire id", "macho64-x86_64-darwin",
         [](nlohmann::json& d) {
             d["macho"]["differenceRelocations"][0]["subtrahendNativeId"] = d["relocations"][0]["nativeId"];
         },
         "/macho/differenceRelocations/0/subtrahendNativeId", "decodes to no RelocationKind"},
        {"one subtrahend names two minuends", "macho64-x86_64-darwin",
         [](nlohmann::json& d) {
             d["macho"]["differenceRelocations"][1]["subtrahendNativeId"] =
                 d["macho"]["differenceRelocations"][0]["subtrahendNativeId"];
         },
         "/macho/differenceRelocations/1/subtrahendNativeId", "one subtrahend names one minuend"},
        {"a subtrahend is another pair's minuend", "macho64-x86_64-darwin",
         [](nlohmann::json& d) {
             d["macho"]["differenceRelocations"][1]["subtrahendNativeId"] =
                 d["macho"]["differenceRelocations"][0]["minuendNativeId"];
         },
         "/macho/differenceRelocations/1/subtrahendNativeId", "both the start of a pair and the end of one"},
        {"one row whose two ids are one", "macho64-arm64-darwin",
         [](nlohmann::json& d) {
             d["macho"]["differenceRelocations"][0]["minuendNativeId"] =
                 d["macho"]["differenceRelocations"][0]["subtrahendNativeId"];
         },
         "/macho/differenceRelocations/0/subtrahendNativeId", "both the start of a pair and the end of one"},
        {"the two ids state different widths", "macho64-arm64-darwin",
         [](nlohmann::json& d) {
             // the 8-byte subtrahend beside the 4-byte pair's minuend
             d["macho"]["differenceRelocations"][0]["minuendNativeId"] =
                 d["macho"]["differenceRelocations"][1]["minuendNativeId"];
         },
         "/macho/differenceRelocations/0/minuendNativeId", "state different widths"},
        {"a pc-relative id", "macho64-arm64-darwin",
         [](nlohmann::json& d) {
             auto& id = d["macho"]["differenceRelocations"][0]["subtrahendNativeId"];
             id = id.get<std::uint32_t>() | (1u << 24);
         },
         "/macho/differenceRelocations/0/subtrahendNativeId", "is pc-relative"},
        {"an id that sets a bit belonging to one entry", "macho64-x86_64-darwin",
         [](nlohmann::json& d) {
             auto& id = d["macho"]["differenceRelocations"][0]["minuendNativeId"];
             id = id.get<std::uint32_t>() | (1u << 27);
         },
         "/macho/differenceRelocations/0/minuendNativeId", "sets bits that belong to ONE relocation entry"},
        {"a width restated beside the ids", "macho64-arm64-darwin",
         [](nlohmann::json& d) { d["macho"]["differenceRelocations"][0]["fieldBytes"] = 8; },
         "/macho/differenceRelocations/0/fieldBytes", "unknown key"},
        {"the table on an image document", "macho64-x86_64-darwin-exec",
         [](nlohmann::json& d) {
             d["macho"]["differenceRelocations"] = nlohmann::json::array(
                 {nlohmann::json{{"subtrahendNativeId", 1409286144}, {"minuendNativeId", 67108864}}});
         },
         "/macho/differenceRelocations/0", "nothing would read the table"},
    };
    for (auto const& cell : kCells) {
        SCOPED_TRACE(cell.label);
        auto const text = shippedFormatText(cell.stem);
        ASSERT_FALSE(text.empty());
        {
            auto const control = ObjectFormatSchema::loadFromText(text, cell.stem);
            ASSERT_TRUE(control.has_value()) << "the unmodified document must load: " << rejectSummary(control);
        }
        nlohmann::json doc = nlohmann::json::parse(text);
        cell.mutate(doc);
        auto const r = ObjectFormatSchema::loadFromText(doc.dump(), cell.stem);
        EXPECT_FALSE(r.has_value()) << "the rule did not fire";
        EXPECT_GE(countAtPath(r, cell.path), 1u) << rejectSummary(r);
        EXPECT_GE(countWithMessage(r, cell.says), 1u) << rejectSummary(r);
    }
}
