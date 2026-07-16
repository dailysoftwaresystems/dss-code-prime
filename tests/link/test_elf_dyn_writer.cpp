// ELF ET_DYN (.so) writer tests — c150, the D-LK1-4 shared-library half.
//
// Pins the shared-library contract a FOREIGN toolchain consumes
// (`gcc main.c -L. -lfoo` links against the DSS-emitted libfoo.so):
//   * e_type = ET_DYN (3), e_entry = 0, base-0 VAs, NO PT_INTERP /
//     PT_PHDR, DT_SONAME iff configured (`elf.soname`).
//   * EXPORTS: externally-visible defined functions + data globals in
//     `.dynsym` (GLOBAL/WEAK DEFAULT, real names, real st_value /
//     st_shndx / st_size) FINDABLE through the SysV `.hash` chain —
//     the strongest pin computes the hash and walks the bucket, the
//     exact lookup ld.so performs.
//   * R_X86_64_RELATIVE for internal absolute data slots (fn-ptr
//     tables): r_addend == the base-relative target VA AND the slot
//     bytes carry the same value (red-on-disable for the RELATIVE
//     emission).
//   * extern FUNCTION imports keep PLT + GLOB_DAT; extern DATA
//     imports bind got-indirect (GOT slot + GLOB_DAT, never COPY).
//   * undefined-extern policy: dyn KEEPS referenced no-library
//     externs (ld.so global scope); exec still REJECTS.
//   * validate() shape rules: no interpreter / no entryPoint / text
//     VA == pageAlign / no copy-relocation binding / soname dyn-only.
//   * fail-loud belts: TLS-in-.so (D-LK-DYN-TLS-MODEL), absolute
//     reloc in `.text` (D-LK-DYN-TEXT-ABS-RELOC), reloc-bearing
//     rodata (D-LK-DYN-RODATA-ITEM-RELOC).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::uint16_t readU16LE(std::span<std::uint8_t const> b,
                                       std::size_t off) {
    return static_cast<std::uint16_t>(b[off])
         | (static_cast<std::uint16_t>(b[off + 1]) << 8);
}
[[nodiscard]] std::uint32_t readU32LE(std::span<std::uint8_t const> b,
                                       std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(b[off + i]) << (i * 8);
    return v;
}
[[nodiscard]] std::uint64_t readU64LE(std::span<std::uint8_t const> b,
                                       std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    return v;
}

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShippedDyn() {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(x86_64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-dyn");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(elf64-x86_64-linux-dyn) failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

// ── Parsed-image helpers ─────────────────────────────────────────

struct Shdr {
    std::string   name;
    std::uint32_t type = 0;
    std::uint64_t flags = 0, addr = 0, offset = 0, size = 0;
    std::uint32_t link = 0;
    std::uint64_t entsize = 0;
};

[[nodiscard]] std::string readCStr(std::vector<std::uint8_t> const& b,
                                   std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p)
        s.push_back(static_cast<char>(b[p]));
    return s;
}

[[nodiscard]] std::vector<Shdr> readSections(std::vector<std::uint8_t> const& b) {
    std::uint64_t const shoff    = readU64LE(b, 40);
    std::uint16_t const shnum    = readU16LE(b, 60);
    std::uint16_t const shstrndx = readU16LE(b, 62);
    std::uint64_t const strOff   = readU64LE(b, shoff + shstrndx * 64 + 24);
    std::vector<Shdr> out;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const off = shoff + static_cast<std::uint64_t>(i) * 64;
        Shdr s;
        s.name    = readCStr(b, strOff + readU32LE(b, off + 0));
        s.type    = readU32LE(b, off + 4);
        s.flags   = readU64LE(b, off + 8);
        s.addr    = readU64LE(b, off + 16);
        s.offset  = readU64LE(b, off + 24);
        s.size    = readU64LE(b, off + 32);
        s.link    = readU32LE(b, off + 40);
        s.entsize = readU64LE(b, off + 56);
        out.push_back(std::move(s));
    }
    return out;
}

[[nodiscard]] Shdr const* findSection(std::vector<Shdr> const& secs,
                                      std::string const& name) {
    for (auto const& s : secs) if (s.name == name) return &s;
    return nullptr;
}

// All (d_tag, d_val) pairs of `.dynamic`.
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>>
readDynamic(std::vector<std::uint8_t> const& b, std::vector<Shdr> const& secs) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    Shdr const* dyn = findSection(secs, ".dynamic");
    if (dyn == nullptr) return out;
    for (std::uint64_t p = 0; p + 16 <= dyn->size; p += 16) {
        out.emplace_back(readU64LE(b, dyn->offset + p),
                         readU64LE(b, dyn->offset + p + 8));
    }
    return out;
}

[[nodiscard]] std::optional<std::uint64_t>
dynValue(std::vector<std::pair<std::uint64_t, std::uint64_t>> const& d,
         std::uint64_t tag) {
    for (auto const& [t, v] : d) if (t == tag) return v;
    return std::nullopt;
}

struct DynSym {
    std::string   name;
    std::uint8_t  info = 0;
    std::uint16_t shndx = 0;
    std::uint64_t value = 0, size = 0;
};

[[nodiscard]] std::vector<DynSym>
readDynsyms(std::vector<std::uint8_t> const& b, std::vector<Shdr> const& secs) {
    std::vector<DynSym> out;
    Shdr const* ds  = findSection(secs, ".dynsym");
    Shdr const* str = findSection(secs, ".dynstr");
    if (ds == nullptr || str == nullptr) return out;
    for (std::uint64_t p = 0; p + 24 <= ds->size; p += 24) {
        std::uint64_t const off = ds->offset + p;
        DynSym s;
        s.name  = readCStr(b, str->offset + readU32LE(b, off + 0));
        s.info  = b[off + 4];
        s.shndx = readU16LE(b, off + 6);
        s.value = readU64LE(b, off + 8);
        s.size  = readU64LE(b, off + 16);
        out.push_back(std::move(s));
    }
    return out;
}

// The exact SysV ELF hash (gABI figure 5-13) — computed HERE so the
// test walks the emitted `.hash` the same way ld.so does.
[[nodiscard]] std::uint32_t sysvElfHash(std::string const& name) {
    std::uint32_t h = 0;
    for (unsigned char c : name) {
        h = (h << 4) + c;
        std::uint32_t const g = h & 0xF0000000u;
        if (g != 0) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

// Walk `.hash` for `name`: hash -> bucket -> chain, comparing dynsym
// names. Returns the found dynsym index or nullopt — the ld.so lookup.
[[nodiscard]] std::optional<std::uint32_t>
hashLookup(std::vector<std::uint8_t> const& b, std::vector<Shdr> const& secs,
           std::vector<DynSym> const& syms, std::string const& name) {
    Shdr const* hs = findSection(secs, ".hash");
    if (hs == nullptr) return std::nullopt;
    std::uint32_t const nbucket = readU32LE(b, hs->offset + 0);
    std::uint32_t const nchain  = readU32LE(b, hs->offset + 4);
    if (nbucket == 0) return std::nullopt;
    std::uint64_t const bucketsOff = hs->offset + 8;
    std::uint64_t const chainsOff  = bucketsOff + 4ull * nbucket;
    std::uint32_t idx =
        readU32LE(b, bucketsOff + 4ull * (sysvElfHash(name) % nbucket));
    std::uint32_t hops = 0;
    while (idx != 0) {
        if (idx >= nchain || idx >= syms.size() || ++hops > nchain) {
            return std::nullopt;  // corrupt chain — fail the lookup loudly
        }
        if (syms[idx].name == name) return idx;
        idx = readU32LE(b, chainsOff + 4ull * idx);
    }
    return std::nullopt;
}

// ── Module builders ──────────────────────────────────────────────

// One exported function `dss_add` (0xC3 ret body) + one exported int
// global `dss_global` (.data {7,0,0,0}) + one LOCAL (static) function
// that must NOT export.
[[nodiscard]] AssembledModule makeExportModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledFunction loc;
    loc.symbol = SymbolId{2};
    loc.bytes  = {0x90, 0xC3};
    mod.functions.push_back(std::move(loc));
    AssembledData d;
    d.symbol    = SymbolId{3};
    d.section   = DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_add",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{2}, "hidden_helper",
                                       SymbolBinding::Local,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{3}, "dss_global",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

// One function + one libc extern FUNCTION import with a `call rel32`
// reloc against it (kind 1 = PC32 on the shipped schemas).
[[nodiscard]] AssembledModule makeExternCallModule(std::string library) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};   // call rel32; ret
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{99};
    rel.kind   = RelocationKind{1};
    rel.addend = -4;
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "puts";
    imp.libraryPath = std::move(library);
    mod.externImports.push_back(std::move(imp));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_add",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

// One function `dss_dispatch` + a RELRO fn-ptr table {&f} — the W2
// shape: the const table slot carries an abs64 (kind 2) reloc to the
// function; the dyn image must emit R_X86_64_RELATIVE for it.
[[nodiscard]] AssembledModule makeFnPtrTableModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledData tab;
    tab.symbol    = SymbolId{5};
    tab.section   = DataSectionKind::RelRoConst;
    tab.bytes     = std::vector<std::uint8_t>(8, 0);
    tab.alignment = Alignment::of<8>();
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{2};   // abs64
    rel.addend = 0;
    tab.relocations.push_back(rel);
    mod.dataItems.push_back(std::move(tab));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_dispatch",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{5}, "tab",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

constexpr std::uint64_t kDtNeeded = 1;
constexpr std::uint64_t kDtSoname = 14;
constexpr std::uint64_t kDtRela   = 7;
constexpr std::uint64_t kDtRelasz = 8;
constexpr std::uint32_t kPtInterp = 3;
constexpr std::uint32_t kPtPhdr   = 6;
constexpr std::uint32_t kRGlobDat = 6;
constexpr std::uint32_t kRCopy    = 5;
constexpr std::uint32_t kRRelative = 8;
constexpr std::uint32_t kRAbs64   = 1;   // R_X86_64_64 — symbol-based absolute

// All (r_offset, symIdx, type, addend) rows of `.rela.dyn`.
struct RelaRow {
    std::uint64_t offset = 0;
    std::uint32_t sym = 0, type = 0;
    std::int64_t  addend = 0;
};
[[nodiscard]] std::vector<RelaRow>
readRelaDyn(std::vector<std::uint8_t> const& b, std::vector<Shdr> const& secs) {
    std::vector<RelaRow> out;
    Shdr const* ra = findSection(secs, ".rela.dyn");
    if (ra == nullptr) return out;
    for (std::uint64_t p = 0; p + 24 <= ra->size; p += 24) {
        std::uint64_t const off  = ra->offset + p;
        std::uint64_t const info = readU64LE(b, off + 8);
        out.push_back(RelaRow{
            readU64LE(b, off + 0),
            static_cast<std::uint32_t>(info >> 32),
            static_cast<std::uint32_t>(info & 0xFFFFFFFFull),
            static_cast<std::int64_t>(readU64LE(b, off + 16))});
    }
    return out;
}

} // namespace

// ── Schema shape (validate + accessors) ─────────────────────────

TEST(ElfDynFormatJson, ShippedDynFileLoadsWithDynShape) {
    auto loaded = loadShippedDyn();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::Elf);
    EXPECT_EQ(loaded.format->name(), "elf64-x86_64-linux-dyn");
    EXPECT_EQ(loaded.format->elf().objectType, ElfObjectType::Dyn);
    // c150: ET_DYN IS an image flavor (load-time-bound) AND allows
    // undefined imports (ld.so global scope) — the two predicates
    // deliberately diverge here.
    EXPECT_TRUE(loaded.format->isImageFlavor());
    EXPECT_TRUE(loaded.format->allowsUndefinedImports());
    // The exec sibling: image flavor, but undefined imports REJECT.
    auto execFmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(execFmt.has_value());
    EXPECT_TRUE((*execFmt)->isImageFlavor());
    EXPECT_FALSE((*execFmt)->allowsUndefinedImports());
    // The rel sibling: NOT an image, undefined imports allowed (c143).
    auto relFmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(relFmt.has_value());
    EXPECT_FALSE((*relFmt)->isImageFlavor());
    EXPECT_TRUE((*relFmt)->allowsUndefinedImports());
    // Dyn shape fields.
    EXPECT_TRUE(loaded.format->elf().interpreter.empty());
    EXPECT_TRUE(loaded.format->elf().soname.empty());
    EXPECT_EQ(loaded.format->entryPoint(), "");
    EXPECT_FALSE(loaded.format->processExit().has_value());
    ASSERT_TRUE(loaded.format->dataImportBinding().has_value());
    EXPECT_EQ(*loaded.format->dataImportBinding(),
              DataImportBinding::GotIndirect);
    auto const* secText = loaded.format->sectionByKind(SectionKind::Text);
    ASSERT_NE(secText, nullptr);
    // Base-0 by construction: text VA == pageAlign.
    EXPECT_EQ(secText->virtualAddress, loaded.format->elf().pageAlign);
}

TEST(ElfDynFormatJson, InterpreterOnDynRejectedAtLoad) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "dataModel": "LP64",
      "format": {"name":"dyn-with-interp","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"dyn", "pageAlign": 4096, "interpreter": "/lib64/ld-linux-x86-64.so.2" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfDynFormatJson, EntryPointOnDynRejectedAtLoad) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "dataModel": "LP64",
      "format": {"name":"dyn-with-entry","kind":"elf"},
      "entryPoint": "main",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"dyn", "pageAlign": 4096 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfDynFormatJson, TextVaNotEqualPageAlignRejectedAtLoad) {
    // ET_DYN is base-0 by construction: text VA must equal pageAlign
    // (the exec-style 0x401000 here must reject).
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "dataModel": "LP64",
      "format": {"name":"dyn-exec-va","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"dyn", "pageAlign": 4096 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfDynFormatJson, CopyRelocationBindingOnDynRejectedAtLoad) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "dataModel": "LP64",
      "format": {"name":"dyn-copy-reloc","kind":"elf"},
      "dataImportBinding": "copy-relocation",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"dyn", "pageAlign": 4096 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfDynFormatJson, SonameOnNonDynRejectedAtLoad) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "dataModel": "LP64",
      "format": {"name":"exec-with-soname","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096, "soname": "libx.so.1" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── Writer: header + phdr pins ──────────────────────────────────

TEST(ElfDynWriter, HeaderPinsEtDynZeroEntryNoInterpNoSoname) {
    auto loaded = loadShippedDyn();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeExternCallModule("libc.so.6");
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(readU16LE(bytes, 16), 3u) << "e_type must be ET_DYN";
    EXPECT_EQ(readU64LE(bytes, 24), 0u) << "e_entry must be 0 for a .so";
    // Program headers: LOAD + LOAD + DYNAMIC, no PT_INTERP / PT_PHDR.
    std::uint64_t const phoff = readU64LE(bytes, 32);
    std::uint16_t const phnum = readU16LE(bytes, 56);
    EXPECT_EQ(phnum, 3u);
    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::uint32_t const pType = readU32LE(bytes, phoff + i * 56ull);
        EXPECT_NE(pType, kPtInterp) << "a .so must carry no PT_INTERP";
        EXPECT_NE(pType, kPtPhdr)   << "a .so carries no PT_PHDR";
    }
    // First PT_LOAD maps from VA 0 (base-relative image).
    EXPECT_EQ(readU32LE(bytes, phoff + 0), 1u) << "PT_LOAD first";
    EXPECT_EQ(readU64LE(bytes, phoff + 16), 0u) << "base-0 p_vaddr";
    // DT_SONAME absent when not configured; DT_NEEDED libc present.
    auto const secs = readSections(bytes);
    auto const dyn = readDynamic(bytes, secs);
    ASSERT_FALSE(dyn.empty());
    EXPECT_FALSE(dynValue(dyn, kDtSoname).has_value())
        << "no soname configured -> no DT_SONAME";
    auto const needed = dynValue(dyn, kDtNeeded);
    ASSERT_TRUE(needed.has_value());
    Shdr const* dynstr = findSection(secs, ".dynstr");
    ASSERT_NE(dynstr, nullptr);
    EXPECT_EQ(readCStr(bytes, dynstr->offset + *needed), "libc.so.6");
}

TEST(ElfDynWriter, SonameEmittedWhenConfigured) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "dataModel": "LP64",
      "format": {"name":"dyn-with-soname","kind":"elf"},
      "externCallDispatch": "direct-plt",
      "supportedDataSections": ["rodata", "data", "bss", "relro"],
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"dyn", "pageAlign": 4096, "soname": "libdss.so.1" },
      "sections":[
        {"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4096},
        {"kind":"data","name":".data","type":1,"flags":3,"addrAlign":8,"entrySize":0,"virtualAddress":0},
        {"kind":"symtab","name":".symtab","type":2,"flags":0,"addrAlign":8,"entrySize":24,"virtualAddress":0},
        {"kind":"strtab","name":".strtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0},
        {"kind":"shstrtab","name":".shstrtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0}
      ],
      "relocations":[{"name":"R_X86_64_PC32","kind":1,"nativeId":2}]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeExternCallModule("libc.so.6");
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    auto const secs = readSections(bytes);
    auto const dyn = readDynamic(bytes, secs);
    auto const soname = dynValue(dyn, kDtSoname);
    ASSERT_TRUE(soname.has_value()) << "elf.soname configured -> DT_SONAME";
    Shdr const* dynstr = findSection(secs, ".dynstr");
    ASSERT_NE(dynstr, nullptr);
    EXPECT_EQ(readCStr(bytes, dynstr->offset + *soname), "libdss.so.1");
}

// ── Exports: .dynsym + SysV .hash findability ───────────────────

TEST(ElfDynWriter, ExportsFindableThroughSysvHashChain) {
    auto loaded = loadShippedDyn();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeExportModule();
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    auto const secs = readSections(bytes);
    auto const syms = readDynsyms(bytes, secs);
    ASSERT_GE(syms.size(), 3u) << "STN_UNDEF + dss_add + dss_global";

    // The ld.so lookup: hash the name, walk bucket + chain.
    auto const fnIdx = hashLookup(bytes, secs, syms, "dss_add");
    ASSERT_TRUE(fnIdx.has_value())
        << "dss_add must be FINDABLE via the SysV hash chain";
    DynSym const& fn = syms[*fnIdx];
    EXPECT_EQ(fn.info, 0x12u) << "STB_GLOBAL<<4 | STT_FUNC";
    EXPECT_NE(fn.shndx, 0u)  << "defined (real section), not UND";
    EXPECT_EQ(fn.value, 0x1000u) << "first function at .text VA (base-0)";
    EXPECT_EQ(fn.size, 1u) << "st_size = the ret body";

    auto const dataIdx = hashLookup(bytes, secs, syms, "dss_global");
    ASSERT_TRUE(dataIdx.has_value())
        << "dss_global must be FINDABLE via the SysV hash chain";
    DynSym const& dg = syms[*dataIdx];
    EXPECT_EQ(dg.info, 0x11u) << "STB_GLOBAL<<4 | STT_OBJECT";
    EXPECT_NE(dg.shndx, 0u);
    EXPECT_EQ(dg.size, 4u) << "int global st_size";
    EXPECT_NE(dg.value, 0u);
    // The export's st_value points at the initialized bytes {7,0,0,0}:
    // find the .data section and check the file bytes at the VA delta.
    Shdr const* data = findSection(secs, ".data");
    ASSERT_NE(data, nullptr);
    ASSERT_GE(dg.value, data->addr);
    std::uint64_t const fileOff = data->offset + (dg.value - data->addr);
    EXPECT_EQ(bytes[fileOff + 0], 7u);
    EXPECT_EQ(bytes[fileOff + 1], 0u);

    // The LOCAL (static) helper must NOT be exported.
    ASSERT_FALSE(hashLookup(bytes, secs, syms, "hidden_helper").has_value());
    for (auto const& s : syms) EXPECT_NE(s.name, "hidden_helper");
}

// ── R_X86_64_RELATIVE for internal fn-ptr slots ─────────────────

TEST(ElfDynWriter, FnPtrTableSlotEmitsRelativeWithBaseRelativeAddend) {
    auto loaded = loadShippedDyn();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeFnPtrTableModule();
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    auto const secs = readSections(bytes);
    auto const rela = readRelaDyn(bytes, secs);
    // RED-ON-DISABLE: dropping the RELATIVE emission empties this.
    std::vector<RelaRow> relatives;
    for (auto const& r : rela) if (r.type == kRRelative) relatives.push_back(r);
    ASSERT_EQ(relatives.size(), 1u)
        << "one abs64 fn-ptr slot -> exactly one R_X86_64_RELATIVE";
    RelaRow const& r = relatives[0];
    EXPECT_EQ(r.sym, 0u) << "RELATIVE rows carry no symbol";
    // The function sits at the base-relative .text VA (0x1000); the
    // addend must be that base-relative target.
    EXPECT_EQ(r.addend, 0x1000) << "r_addend = base-relative fn VA";
    // The slot itself lives in the writable .data (relro merged there)
    // and its bytes carry the SAME base-relative value (the prelinked
    // slot convention; ld.so recomputes base + addend over it).
    Shdr const* data = findSection(secs, ".data");
    ASSERT_NE(data, nullptr);
    ASSERT_GE(r.offset, data->addr);
    ASSERT_LT(r.offset + 8, data->addr + data->size + 1);
    std::uint64_t const slotFileOff = data->offset + (r.offset - data->addr);
    EXPECT_EQ(readU64LE(bytes, slotFileOff),
              static_cast<std::uint64_t>(r.addend))
        << "slot bytes == r_addend (base-relative)";
    // DT_RELA/DT_RELASZ cover the row.
    auto const dyn = readDynamic(bytes, secs);
    auto const relasz = dynValue(dyn, kDtRelasz);
    ASSERT_TRUE(dynValue(dyn, kDtRela).has_value());
    ASSERT_TRUE(relasz.has_value());
    EXPECT_EQ(*relasz, 24u);
}

// ── Extern-ADDRESS static initializers (c150 review-fold CRITICAL) ──

// A data slot whose reloc targets an EXTERN must emit a SYMBOL-BASED
// absolute reloc (R_X86_64_64 <dynsym> + rel.addend) with the slot bytes
// ZEROED — never a RELATIVE row. The apply patches such a slot with
// `symbolVa[extern]` (the GOT SLOT VA for a data extern / the local PLT
// STUB VA for a function extern); a RELATIVE row would bake load_base +
// that address into the pointer — `FILE **pp = &stdout;` pointed at the
// .so's OWN GOT SLOT (one indirection off, witnessed live at runtime), and
// `int (*fp)() = puts;` bound to the local stub (cross-module `fp == puts`
// false, C11 6.5.9). gcc emits exactly the symbol-based shape. Covers BOTH
// extern kinds in one module + an internal control slot that must STAY
// RELATIVE. RED-ON-DISABLE: drop the extern-site discrimination -> all
// three rows come back RELATIVE and the slot bytes carry the slot/stub VAs.
TEST(ElfDynWriter, ExternAddressDataSlotEmitsSymbolBasedAbs64NotRelative) {
    auto loaded = loadShippedDyn();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_fn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    ExternImport dataExt;                       // `stdout` shape
    dataExt.symbol      = SymbolId{80};
    dataExt.mangledName = "stdout";
    dataExt.libraryPath = "libc.so.6";
    dataExt.isData      = true;
    mod.externImports.push_back(std::move(dataExt));
    ExternImport fnExt;                         // `puts` shape
    fnExt.symbol      = SymbolId{81};
    fnExt.mangledName = "puts";
    fnExt.libraryPath = "libc.so.6";
    fnExt.isData      = false;
    mod.externImports.push_back(std::move(fnExt));
    AssembledData tab;                          // three 8-byte pointer slots
    tab.symbol    = SymbolId{5};
    tab.section   = DataSectionKind::Data;
    tab.bytes     = std::vector<std::uint8_t>(24, 0);
    tab.alignment = Alignment::of<8>();
    tab.relocations.push_back(Relocation{0u,  SymbolId{80},
                                         RelocationKind{2}, /*addend=*/0});
    tab.relocations.push_back(Relocation{8u,  SymbolId{81},
                                         RelocationKind{2}, /*addend=*/0});
    tab.relocations.push_back(Relocation{16u, SymbolId{1},
                                         RelocationKind{2}, /*addend=*/0});
    mod.dataItems.push_back(std::move(tab));
    mod.symbols.push_back(ModuleSymbol{SymbolId{5}, "ptab",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    auto const secs = readSections(bytes);
    auto const rela = readRelaDyn(bytes, secs);
    Shdr const* data = findSection(secs, ".data");
    ASSERT_NE(data, nullptr);

    std::vector<RelaRow> abs64Rows, relativeRows;
    for (auto const& r : rela) {
        if (r.type == kRAbs64) abs64Rows.push_back(r);
        if (r.type == kRRelative) relativeRows.push_back(r);
    }
    ASSERT_EQ(abs64Rows.size(), 2u)
        << "the two extern-targeted slots -> symbol-based R_X86_64_64 rows";
    ASSERT_EQ(relativeRows.size(), 1u)
        << "the internal-targeted slot stays RELATIVE";
    // Slots sit at .data addr + 0/8/16 (first item in the layout).
    std::uint64_t const slot0 = data->addr + 0, slot1 = data->addr + 8,
                        slot2 = data->addr + 16;
    EXPECT_EQ(abs64Rows[0].offset, slot0);
    EXPECT_EQ(abs64Rows[1].offset, slot1);
    EXPECT_EQ(relativeRows[0].offset, slot2);
    // Symbol-based rows: NON-zero dynsym index, DISTINCT symbols, addend 0.
    EXPECT_NE(abs64Rows[0].sym, 0u);
    EXPECT_NE(abs64Rows[1].sym, 0u);
    EXPECT_NE(abs64Rows[0].sym, abs64Rows[1].sym);
    EXPECT_EQ(abs64Rows[0].addend, 0);
    EXPECT_EQ(abs64Rows[1].addend, 0);
    // The extern-targeted slot BYTES are ZERO (ld.so writes the resolved
    // address; a non-zero value here is the slot/stub-VA bake this test
    // guards). The internal slot keeps the prelinked base-relative value.
    auto slotBytes = [&](std::uint64_t va) {
        return readU64LE(bytes, data->offset + (va - data->addr));
    };
    EXPECT_EQ(slotBytes(slot0), 0u) << "extern-data slot must be zeroed";
    EXPECT_EQ(slotBytes(slot1), 0u) << "extern-fn slot must be zeroed";
    EXPECT_EQ(slotBytes(slot2),
              static_cast<std::uint64_t>(relativeRows[0].addend))
        << "internal slot keeps the prelinked RELATIVE convention";
    // Also: neither abs64 row's addend/slot may equal a GOT/PLT-region VA
    // (the exact bake-the-slot break): the GOT lives outside .data's item
    // span here, and zero-checks above already preclude it.
}

// ── Extern imports on the dyn arm ───────────────────────────────

TEST(ElfDynWriter, ExternFunctionKeepsPltAndGlobDat) {
    auto loaded = loadShippedDyn();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeExternCallModule("libc.so.6");
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    auto const secs = readSections(bytes);
    Shdr const* plt = findSection(secs, ".plt");
    ASSERT_NE(plt, nullptr);
    EXPECT_EQ(plt->size, 6u) << "one x86_64 PLT stub (FF 25 disp32)";
    auto const rela = readRelaDyn(bytes, secs);
    ASSERT_EQ(rela.size(), 1u);
    EXPECT_EQ(rela[0].type, kRGlobDat) << "GLOB_DAT against the GOT slot";
    auto const syms = readDynsyms(bytes, secs);
    ASSERT_GT(rela[0].sym, 0u);
    ASSERT_LT(rela[0].sym, syms.size());
    EXPECT_EQ(syms[rela[0].sym].name, "puts");
    EXPECT_EQ(syms[rela[0].sym].shndx, 0u) << "import stays UNDEF";
    // puts is also findable via the hash (ld.so needs that for
    // resolution bookkeeping).
    EXPECT_TRUE(hashLookup(bytes, secs, syms, "puts").has_value());
}

TEST(ElfDynWriter, ExternDataBindsGotIndirectNeverCopy) {
    auto loaded = loadShippedDyn();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // lea rax,[rip+disp32]; ret — the got-indirect lowering's
    // slot-address load, reloc'd to the extern's GOT slot (kind 1).
    fn.bytes = {0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 3;
    rel.target = SymbolId{77};
    rel.kind   = RelocationKind{1};
    rel.addend = -4;
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{77};
    imp.mangledName = "stdout";
    imp.libraryPath = "libc.so.6";
    imp.isData      = true;
    imp.dataSizeBytes  = 8;
    imp.dataAlignBytes = 8;
    mod.externImports.push_back(std::move(imp));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "use_stdout",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    auto const secs = readSections(bytes);
    auto const rela = readRelaDyn(bytes, secs);
    ASSERT_EQ(rela.size(), 1u);
    EXPECT_EQ(rela[0].type, kRGlobDat)
        << "dyn data import binds got-indirect (GLOB_DAT), never COPY";
    EXPECT_NE(rela[0].type, kRCopy);
    Shdr const* got = findSection(secs, ".got");
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->size, 8u) << "one GOT slot for the data import";
    EXPECT_GE(rela[0].offset, got->addr);
    EXPECT_LT(rela[0].offset, got->addr + got->size);
    auto const syms = readDynsyms(bytes, secs);
    ASSERT_LT(rela[0].sym, syms.size());
    EXPECT_EQ(syms[rela[0].sym].name, "stdout");
    EXPECT_EQ(syms[rela[0].sym].shndx, 0u) << "UNDEF import, no copy slot";
    EXPECT_EQ(syms[rela[0].sym].value, 0u);
}

// ── Self-contained .so (zero externs) ───────────────────────────

TEST(ElfDynWriter, ZeroExternSelfContainedSoStillEmitsDynamicMetadata) {
    auto loaded = loadShippedDyn();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeExportModule();   // no externImports
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(readU16LE(bytes, 16), 3u) << "ET_DYN";
    auto const secs = readSections(bytes);
    auto const dyn = readDynamic(bytes, secs);
    ASSERT_FALSE(dyn.empty()) << ".dynamic must exist for exports";
    EXPECT_FALSE(dynValue(dyn, kDtNeeded).has_value())
        << "no externs -> no DT_NEEDED";
    EXPECT_FALSE(dynValue(dyn, kDtRela).has_value())
        << "no externs + no RELATIVE slots -> no DT_RELA trio";
    auto const syms = readDynsyms(bytes, secs);
    EXPECT_TRUE(hashLookup(bytes, secs, syms, "dss_add").has_value());
}

// ── Undefined-extern policy (the c143 gate's third flavor) ──────

TEST(ElfDynLinker, ReferencedNoLibraryExternKeptForDynRejectedForExec) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto dynFmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(dynFmt.has_value());
    auto execFmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(execFmt.has_value());

    // dyn: the referenced no-library extern is KEPT (ld -shared
    // semantics — ld.so resolves it from the global scope at load).
    {
        AssembledModule mod = makeExternCallModule("");
        DiagnosticReporter rep;
        auto image = linker::link(mod, **target, **dynFmt, rep);
        EXPECT_EQ(rep.errorCount(), 0u)
            << "a .so may carry undefined externs (standard ld -shared)";
        ASSERT_TRUE(image.ok());
        ASSERT_FALSE(image.bytes.empty());
        // The UNDEF symbol is present + findable in the emitted .so.
        auto const secs = readSections(image.bytes);
        auto const syms = readDynsyms(image.bytes, secs);
        auto const idx = hashLookup(image.bytes, secs, syms, "puts");
        ASSERT_TRUE(idx.has_value());
        EXPECT_EQ(syms[*idx].shndx, 0u) << "kept as UNDEF";
        auto const dyn = readDynamic(image.bytes, secs);
        EXPECT_FALSE(dynValue(dyn, kDtNeeded).has_value())
            << "no library -> no DT_NEEDED row";
    }
    // exec: unchanged c143 policy — reject loud.
    {
        AssembledModule mod = makeExternCallModule("");
        DiagnosticReporter rep;
        auto image = linker::link(mod, **target, **execFmt, rep);
        EXPECT_FALSE(image.ok());
        bool sawUndef = false;
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::K_SymbolUndefined
                && d.actual.find("puts") != std::string::npos) {
                sawUndef = true;
            }
        }
        EXPECT_TRUE(sawUndef)
            << "an exec image still rejects referenced no-library externs";
    }
}

// ── Fail-loud belts ─────────────────────────────────────────────

TEST(ElfDynWriter, TlsItemFailsLoudOnDynArm) {
    // Walker belt (D-LK-DYN-TLS-MODEL): a hand-built module with a
    // thread-local item reaching the dyn walker fails loud — local-
    // exec tpoffs are exec-only physics.
    auto loaded = loadShippedDyn();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeExportModule();
    AssembledData tls;
    tls.symbol    = SymbolId{9};
    tls.section   = DataSectionKind::Tdata;
    tls.bytes     = {1, 0, 0, 0};
    tls.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(tls));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksThreadLocalSupport
            && d.actual.find("D-LK-DYN-TLS-MODEL") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}

TEST(ElfDynWriter, AbsoluteTextRelocFailsLoudOnDynArm) {
    // Walker belt (D-LK-DYN-TEXT-ABS-RELOC): an absolute fixup in the
    // `.text` of a slid image is DT_TEXTREL territory — fail loud.
    auto loaded = loadShippedDyn();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = std::vector<std::uint8_t>(10, 0x90);
    Relocation rel;
    rel.offset = 2;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{2};   // abs64 IN TEXT — invalid for .so
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "f",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch
            && d.actual.find("D-LK-DYN-TEXT-ABS-RELOC") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}

TEST(ElfDynWriter, RelocBearingRodataFailsLoudOnDynArm) {
    // Walker belt (D-LK-DYN-RODATA-ITEM-RELOC): a reloc-bearing item
    // in the READ-ONLY rodata cannot take a load-time RELATIVE write.
    auto loaded = loadShippedDyn();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledData d;
    d.symbol    = SymbolId{5};
    d.section   = DataSectionKind::Rodata;   // NOT relro — the belt case
    d.bytes     = std::vector<std::uint8_t>(8, 0);
    d.alignment = Alignment::of<8>();
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{2};
    d.relocations.push_back(rel);
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "f",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d2 : rep.all()) {
        if (d2.code == DiagnosticCode::K_RelocationKindMismatch
            && d2.actual.find("D-LK-DYN-RODATA-ITEM-RELOC")
                   != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}
