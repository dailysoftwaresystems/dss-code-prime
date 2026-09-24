// ★★★ A SIBLING MODULE'S DATUM, READ THROUGH A SLOT BY THE CODE THAT IMPORTS IT
// (D-LK-SIBLING-DATA-IMPORT-SLOT-BOUND-TO-THE-OBJECT, P68 round 9, routed from
// lane `lm`).
//
// Every image format declares `dataImportBinding: got-indirect`, so MIR→LIR
// lowers each CODE reference to an extern DATUM as `lea <import>` + a load of
// the POINTER there. When the datum is defined by a SIBLING module of the link
// (a pulled static-archive member), the merge binds the import to it — and it
// used to bind the whole reference to the OBJECT itself, so the code loaded the
// object's first bytes as a pointer. ✔MEASURED 2026-09-24 at the round's base:
// `int dss_data_answer = 42;` in a DSS static archive, `return dss_data_answer;`
// in main → an access violation on pe64, SIGSEGV (exit 139) on ELF x86_64 and
// ELF aarch64 (qemu).
//
// The fix has two halves, and each is pinned here against the modules a real
// build produces:
//   * WHO SAYS the code reads through a slot: MIR→LIR, by stamping
//     `ExternImport::readThroughSlot` (pinned in `test_mir_to_lir.cpp`). The
//     merge reads it and never infers it: a pulled member's own code reads its
//     extern datum DIRECTLY, and a slot there would load the ADDRESS.
//   * WHERE each relocation goes, by ROLE: the reading CODE to a minted slot
//     that holds the definition's address — on arm64 BOTH halves of the `adrp`
//     + `add :lo12:` that address it — and a DATA item (`int *p = &x;`) to the
//     definition itself.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include "link_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;
using dss::link_format::test::readU32LE;
using dss::link_format::test::readU64LE;

namespace {

// ── a minimal ELF64 image reader: VA ↔ file offset through PT_LOAD ─────────

struct Load { std::uint64_t off, va, filesz; };

[[nodiscard]] std::vector<Load> loads(std::vector<std::uint8_t> const& img) {
    std::vector<Load> out;
    std::uint64_t const phoff = readU64LE(img, 0x20);
    std::uint16_t const phentsize = static_cast<std::uint16_t>(
        img.at(0x36) | (img.at(0x37) << 8));
    std::uint16_t const phnum = static_cast<std::uint16_t>(
        img.at(0x38) | (img.at(0x39) << 8));
    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::uint64_t const p = phoff + std::uint64_t{i} * phentsize;
        if (readU32LE(img, p) != 1u) continue;  // PT_LOAD
        out.push_back(Load{readU64LE(img, p + 8), readU64LE(img, p + 16),
                           readU64LE(img, p + 32)});
    }
    return out;
}

[[nodiscard]] std::optional<std::uint64_t> vaToOff(std::vector<Load> const& ls,
                                                   std::uint64_t va) {
    for (auto const& l : ls) {
        if (va >= l.va && va < l.va + l.filesz) return l.off + (va - l.va);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> offToVa(std::vector<Load> const& ls,
                                                   std::uint64_t off) {
    for (auto const& l : ls) {
        if (off >= l.off && off < l.off + l.filesz) return l.va + (off - l.off);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t>
find(std::vector<std::uint8_t> const& hay, std::vector<std::uint8_t> const& needle) {
    for (std::uint64_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (hay[i + j] != needle[j]) { match = false; break; }
        }
        if (match) return i;
    }
    return std::nullopt;
}

[[nodiscard]] RelocationKind kindNamed(TargetSchema const& t, char const* name) {
    auto const* r = t.relocationByName(name);
    EXPECT_NE(r, nullptr) << name;
    return r != nullptr ? r->kind : RelocationKind{};
}

// The DEFINING module: the datum `x`, whose first 8 bytes are a recognizable
// marker, so the test can find the definition — and so its ADDRESS — in the
// image. A load of `x` "as a pointer" (the defect) would read the marker.
constexpr std::uint8_t kMarker[] = {0xD5, 0x5D, 0xA7, 0xA0, 0x0D, 0xF0, 0x0D, 0x42};
// The marker right after `px`'s pointer, so the test reads THAT pointer — and
// not whichever other 8 bytes of the image (a symbol's value, say) happen to
// hold x's address.
constexpr std::uint8_t kPxMarker[] = {0x9C, 0x3E, 0x51, 0x7A, 0xB0, 0x0B, 0x1E, 0x55};

[[nodiscard]] AssembledModule definingModule() {
    AssembledModule m;
    m.cuId = CompilationUnitId{2};
    AssembledData d;
    d.symbol = SymbolId{1};
    d.section = DataSectionKind::Data;
    d.bytes.assign(std::begin(kMarker), std::end(kMarker));
    d.bytes.insert(d.bytes.end(), {42, 0, 0, 0});
    m.dataItems.push_back(d);
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "x", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    return m;
}

// The READING module, x86_64: `main` = `lea x(%rip), %rax` (48 8d 05 disp32)
// then the load through it — the code's only reference, one relocation —
// plus a data item `px` holding `&x` (an abs64, followed by `kPxMarker`).
// `readThroughSlot` is the row MIR→LIR stamps (true: its code reads `x`
// through a slot) or a reader leaves (false: the code reads `x` directly —
// here `mov x(%rip), %eax`, 8b 05).
[[nodiscard]] AssembledModule readingModuleX86(TargetSchema const& t,
                                               bool readThroughSlot) {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    if (readThroughSlot) {
        fn.bytes = {0x48, 0x8D, 0x05, 0, 0, 0, 0,   // lea rax, [rip+x]
                    0x8B, 0x00,                     // mov eax, [rax]
                    0xC3};
    } else {
        fn.bytes = {0x8B, 0x05, 0, 0, 0, 0,         // mov eax, [rip+x]
                    0x90, 0x90, 0x90, 0xC3};
    }
    Relocation rel;
    rel.offset = readThroughSlot ? 3u : 2u;
    rel.target = SymbolId{2};
    rel.kind   = kindNamed(t, "rel32");
    rel.addend = 0;
    fn.relocations.push_back(rel);
    m.functions.push_back(fn);
    AssembledData px;
    px.symbol = SymbolId{3};
    px.section = DataSectionKind::Data;
    px.bytes.assign(8, 0);
    px.bytes.insert(px.bytes.end(), std::begin(kPxMarker), std::end(kPxMarker));
    Relocation abs;
    abs.offset = 0;
    abs.target = SymbolId{2};
    abs.kind   = kindNamed(t, "abs64");
    abs.addend = 0;
    px.relocations.push_back(abs);
    m.dataItems.push_back(px);
    ExternImport ext;
    ext.symbol          = SymbolId{2};
    ext.mangledName     = "x";
    ext.isData          = true;
    ext.readThroughSlot = readThroughSlot;
    m.externImports.push_back(ext);
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{3}, "px", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    return m;
}

struct Linked {
    std::vector<std::uint8_t> bytes;
    std::string               why;
};

[[nodiscard]] Linked linkPair(std::vector<AssembledModule> mods,
                              TargetSchema const& t, char const* format) {
    Linked out;
    auto fmt = ObjectFormatSchema::loadShipped(format);
    if (!fmt.has_value()) {
        out.why = "format did not load";
        return out;
    }
    DiagnosticReporter rep;
    auto image = linker::link(std::span<AssembledModule const>{mods.data(), mods.size()},
                              t, **fmt, rep);
    for (auto const& d : rep.all()) out.why += d.actual + "\n";
    if (!rep.hasErrors()) out.bytes = std::move(image.bytes);
    return out;
}

}  // namespace

// ── x86_64: the code reads through a slot; the initializer reads the object ─

TEST(SiblingDataImportSlot, CodeReadingThroughASlotGetsASlotHoldingTheDefinition) {
    auto t = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(t.has_value());
    std::vector<AssembledModule> mods{readingModuleX86(**t, /*readThroughSlot=*/true),
                                      definingModule()};
    auto const img = linkPair(std::move(mods), **t, "elf64-x86_64-linux-exec");
    ASSERT_FALSE(img.bytes.empty()) << img.why;
    auto const ls = loads(img.bytes);
    auto const defOff = find(img.bytes, {std::begin(kMarker), std::end(kMarker)});
    ASSERT_TRUE(defOff.has_value());
    std::uint64_t const defVa = *offToVa(ls, *defOff);  // the item; `x` is the item
    // `main` is found by its tail (the deref and the `ret`), the lea 7 bytes back.
    auto const tail = find(img.bytes, {0x8B, 0x00, 0xC3});
    ASSERT_TRUE(tail.has_value() && *tail >= 7);
    std::uint64_t const leaOffV = *tail - 7;
    auto const leaOff = std::optional<std::uint64_t>{leaOffV};
    ASSERT_EQ(img.bytes[leaOffV], 0x48);
    ASSERT_EQ(img.bytes[leaOffV + 1], 0x8D);
    ASSERT_EQ(img.bytes[leaOffV + 2], 0x05);
    std::uint64_t const leaVa = *offToVa(ls, *leaOff);
    auto const disp = static_cast<std::int32_t>(readU32LE(img.bytes, *leaOff + 3));
    std::uint64_t const slotVa = leaVa + 7 + static_cast<std::int64_t>(disp);
    ASSERT_NE(slotVa, defVa)
        << "the code loads a POINTER at the address it computes; with the object "
           "itself there it loads 42 as an address (the base's SIGSEGV)";
    auto const slotOff = vaToOff(ls, slotVa);
    ASSERT_TRUE(slotOff.has_value());
    EXPECT_EQ(readU64LE(img.bytes, *slotOff), defVa)
        << "the slot must hold the definition's address";
    // The DATA initializer `px = &x` names the OBJECT, never the slot. `px` is
    // read where its marker says it is.
    auto const pxMark = find(img.bytes, {std::begin(kPxMarker), std::end(kPxMarker)});
    ASSERT_TRUE(pxMark.has_value() && *pxMark >= 8);
    EXPECT_EQ(readU64LE(img.bytes, *pxMark - 8), defVa)
        << "`int *px = &x;` must hold x's own address, not the slot's";
}

// ── a pulled member's code reads its extern datum DIRECTLY: no slot ─────────

TEST(SiblingDataImportSlot, CodeReadingTheDatumDirectlyIsNeverSentToASlot) {
    // A reader's row never states a slot read: a relocatable object's code says
    // how it reaches its imports. An inferred merge ("data, and an image format
    // whose binding is got-indirect") would send this DIRECT load to a slot and
    // load the ADDRESS of x instead of 42.
    auto t = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(t.has_value());
    std::vector<AssembledModule> mods{readingModuleX86(**t, /*readThroughSlot=*/false),
                                      definingModule()};
    auto const img = linkPair(std::move(mods), **t, "elf64-x86_64-linux-exec");
    ASSERT_FALSE(img.bytes.empty()) << img.why;
    auto const ls = loads(img.bytes);
    auto const defOff = find(img.bytes, {std::begin(kMarker), std::end(kMarker)});
    ASSERT_TRUE(defOff.has_value());
    std::uint64_t const defVa = *offToVa(ls, *defOff);
    // `main` is found by its tail (the three nops and the `ret`), the load 6
    // bytes back.
    auto const tail = find(img.bytes, {0x90, 0x90, 0x90, 0xC3});
    ASSERT_TRUE(tail.has_value() && *tail >= 6);
    auto const movOff = std::optional<std::uint64_t>{*tail - 6};
    ASSERT_EQ(img.bytes[*movOff], 0x8B);
    ASSERT_EQ(img.bytes[*movOff + 1], 0x05);
    std::uint64_t const movVa = *offToVa(ls, *movOff);
    auto const disp = static_cast<std::int32_t>(readU32LE(img.bytes, *movOff + 2));
    EXPECT_EQ(movVa + 6 + static_cast<std::int64_t>(disp), defVa)
        << "a direct load of a sibling's datum reads the datum itself";
}

// ── arm64: BOTH halves of the slot's address go to the slot ─────────────────

TEST(SiblingDataImportSlot, Arm64PageAndPageOffsetBothAddressTheSlot) {
    // `adrp x8, x` + `add x8, x8, :lo12:x` + `ldr x8, [x8]` + `ldr w0, [x8]`:
    // the ADRP is PC-relative and the ADD's page offset is not. Retargeting
    // only the PC-relative half would join the slot's page to the definition's
    // page offset — an address that is neither.
    auto t = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(t.has_value());
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes = {0x08, 0x00, 0x00, 0x90,   // adrp x8, x
                0x08, 0x01, 0x00, 0x91,   // add  x8, x8, :lo12:x
                0x08, 0x01, 0x40, 0xF9,   // ldr  x8, [x8]
                0x00, 0x01, 0x40, 0xB9,   // ldr  w0, [x8]
                0xC0, 0x03, 0x5F, 0xD6};  // ret
    fn.relocations.push_back(Relocation{0, SymbolId{2}, kindNamed(**t, "adr_prel_pg_hi21"), 0});
    fn.relocations.push_back(Relocation{4, SymbolId{2}, kindNamed(**t, "add_abs_lo12_nc"), 0});
    m.functions.push_back(fn);
    ExternImport ext;
    ext.symbol          = SymbolId{2};
    ext.mangledName     = "x";
    ext.isData          = true;
    ext.readThroughSlot = true;
    m.externImports.push_back(ext);
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    std::vector<AssembledModule> mods{std::move(m), definingModule()};
    auto const img = linkPair(std::move(mods), **t, "elf64-aarch64-linux-exec");
    ASSERT_FALSE(img.bytes.empty()) << img.why;
    auto const ls = loads(img.bytes);
    auto const defOff = find(img.bytes, {std::begin(kMarker), std::end(kMarker)});
    ASSERT_TRUE(defOff.has_value());
    std::uint64_t const defVa = *offToVa(ls, *defOff);
    // `main` is found by its tail (the two loads and the `ret`), the adrp 8
    // bytes before it.
    auto const tail = find(img.bytes, {0x08, 0x01, 0x40, 0xF9, 0x00, 0x01, 0x40, 0xB9,
                                       0xC0, 0x03, 0x5F, 0xD6});
    ASSERT_TRUE(tail.has_value() && *tail >= 8);
    std::uint64_t const insnOff = *tail - 8;
    std::uint64_t const adrpVa = *offToVa(ls, insnOff);
    std::uint32_t const adrp = readU32LE(img.bytes, insnOff);
    std::uint32_t const add  = readU32LE(img.bytes, insnOff + 4);
    std::int64_t immhi = (adrp >> 5) & 0x7FFFF;
    std::int64_t const immlo = (adrp >> 29) & 0x3;
    std::int64_t pages = (immhi << 2) | immlo;
    if (pages & (std::int64_t{1} << 20)) pages -= std::int64_t{1} << 21;
    std::uint64_t const page = (adrpVa & ~std::uint64_t{0xFFF})
                             + static_cast<std::uint64_t>(pages << 12);
    std::uint64_t const addressed = page + ((add >> 10) & 0xFFF);
    ASSERT_NE(addressed, defVa) << "the code must address the slot, not the object";
    auto const slotOff = vaToOff(ls, addressed);
    ASSERT_TRUE(slotOff.has_value())
        << "the page and the page offset must name ONE location — a split pair "
           "names none";
    EXPECT_EQ(readU64LE(img.bytes, *slotOff), defVa)
        << "the slot must hold the definition's address";
}
