// D-LK-ELF-READER-REFUSES-GOTPCREL-BLOCKS-REAL-GLIBC-MEMBERS — the RESOLUTION
// half. `test_glibc_relocation_vocabulary.cpp` proves a real glibc member READS
// once its GOT-slot-relative wire type is declared; this file proves the read
// module then LINKS — THROUGH `linker::link`, the driver's link, which lowers
// every GOT-slot-relative site to a direct reference to a slot it mints, and
// each slot holds the target's resolved address.
// ⚠ P68 round 11: these pins used to call `elf::encode` on the read module
// directly, and so exercised the STATIC ET_EXEC writer's own `.got` — a writer
// ✔MEASURED 2026-09-24 that no program the driver builds reaches (every ELF
// image imports libc `exit`), while the dynamic writer the driver does reach
// refused the reference. The row was reopened for exactly that; the pins now
// stand where the driver stands.
//
// ⚠ REAL ARCHIVE BYTES, NEVER A SYNTHESISED OBJECT. The fixtures in
// `glibc_relocation_vocabulary_members.inc` are verbatim members of
// `/usr/lib/x86_64-linux-gnu/libc.a` (Ubuntu 24.04, glibc built by gcc 13.3.0,
// archive md5 `56a6e057fd9df0ebce6e3bd15d33b0d9`, 2211 members): `exit.o`
// (md5 `9ec0be271a1766867f0b8425fcf5ee1a`, 3200 bytes) and `uselocale.o`. A
// DSS-emitted object is written by the same table the reader reads back, so it
// can never carry a wire type DSS has not declared and can never exercise this.
//
// ── WHAT EACH TEST HERE IS FOR ────────────────────────────────────────────
//
//  1. `RealExitMemberGotPcRelSitesResolveThroughSlotsTheLinkMints`
//     THE CLOSURE. Linked through `linker::link` (and so through the DYNAMIC
//     writer, every ELF image's), the real member's two
//     `cmpq $0x0,sym@GOTPCREL(%rip)` sites are patched with displacements that
//     land on two DISTINCT slots inside the image's writable data, and the slot
//     each names holds that symbol's link-time address. Every number is
//     recomputed from the EMITTED bytes.
//
//  2. `AModuleWithNoGotSlotRelocationGetsNoSlot`
//     THE CONTROL for (1), and it is not optional: without it, "a slot exists"
//     is equally consistent with "the link always mints one". A foreign gcc
//     `.o` whose every relocation is an ordinary PC32/PLT32 leaves the lowering
//     with nothing to do, and still links.
//
//  3. `AStillUndeclaredWireTypeIsStillRefused`
//     THE GUARD, re-pinned in the SAME commit that removes the apply refusal.
//     Resolving one relocation must not turn the reader permissive: R_X86_64_-
//     GOTTPOFF (22, TLS initial-exec — [[D-LK-DYN-TLS-MODEL]]'s scope, not
//     this row's) is still refused by name.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/got_slots.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include "gcc_lib_c164_object.inc"
#include "glibc_relocation_vocabulary_members.inc"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <algorithm>
#include <string>
#include <vector>

using namespace dss;

namespace {

// R_X86_64_GOTTPOFF — the TLS initial-exec type `uselocale.o` reaches first.
// Named here because test 3 asserts it is STILL undeclared, and a bare 22 in
// an assertion is a number that can drift away from its meaning.
constexpr std::uint32_t kRX8664GotTpOff = 22;

// ── little-endian readers over the emitted image ─────────────────────────
[[nodiscard]] std::uint16_t readU16(std::vector<std::uint8_t> const& b,
                                    std::uint64_t off) {
    return static_cast<std::uint16_t>(b.at(off))
         | static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(b.at(off + 1)) << 8);
}
[[nodiscard]] std::uint32_t readU32(std::vector<std::uint8_t> const& b,
                                    std::uint64_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(b.at(off + i)) << (i * 8);
    return v;
}
[[nodiscard]] std::int32_t readI32(std::vector<std::uint8_t> const& b,
                                   std::uint64_t off) {
    return static_cast<std::int32_t>(readU32(b, off));
}
[[nodiscard]] std::uint64_t readU64(std::vector<std::uint8_t> const& b,
                                    std::uint64_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(b.at(off + i)) << (i * 8);
    return v;
}
[[nodiscard]] std::string readCStr(std::vector<std::uint8_t> const& b,
                                   std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p)
        s.push_back(static_cast<char>(b[p]));
    return s;
}

// One ELF64 section header, read back off the emitted bytes rather than
// remembered from the writer — the whole point is to measure what shipped.
struct Section {
    std::string   name;
    std::uint64_t addr   = 0;
    std::uint64_t offset = 0;
    std::uint64_t size   = 0;
    std::uint64_t entSize = 0;
    std::uint32_t type   = 0;
    std::uint64_t flags  = 0;
    std::uint32_t link   = 0;
};

[[nodiscard]] std::vector<Section>
readSections(std::vector<std::uint8_t> const& b) {
    std::vector<Section> out;
    if (b.size() < 64) return out;
    std::uint64_t const shoff    = readU64(b, 40);
    std::uint16_t const shnum    = readU16(b, 60);
    std::uint16_t const shstrndx = readU16(b, 62);
    std::uint64_t const shstrOff = readU64(b, shoff + shstrndx * 64ull + 24);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const h = shoff + i * 64ull;
        Section s;
        s.name    = readCStr(b, shstrOff + readU32(b, h + 0));
        s.type    = readU32(b, h + 4);
        s.flags   = readU64(b, h + 8);
        s.addr    = readU64(b, h + 16);
        s.offset  = readU64(b, h + 24);
        s.size    = readU64(b, h + 32);
        s.link    = readU32(b, h + 40);
        s.entSize = readU64(b, h + 56);
        out.push_back(std::move(s));
    }
    return out;
}

[[nodiscard]] Section const* findSection(std::vector<Section> const& v,
                                         std::string const& name) {
    for (auto const& s : v) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// `st_value` of a named `.symtab` entry in the emitted image (nullopt when the
// name is absent). Read back off the BYTES, so the assertion cannot be
// satisfied by a number the test itself computed.
[[nodiscard]] std::optional<std::uint64_t>
symbolValue(std::vector<std::uint8_t> const& b, std::string const& want) {
    auto const sections = readSections(b);
    auto const* symtab  = findSection(sections, ".symtab");
    if (symtab == nullptr || symtab->entSize == 0) return std::nullopt;
    if (symtab->link >= sections.size()) return std::nullopt;
    auto const& strtab = sections[symtab->link];
    std::uint64_t const count = symtab->size / symtab->entSize;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t const e = symtab->offset + i * symtab->entSize;
        std::string const name = readCStr(b, strtab.offset + readU32(b, e + 0));
        if (name == want) return readU64(b, e + 8);
    }
    return std::nullopt;
}

// Every `.symtab` name in the emitted image, for a failure message that says
// what DID ship rather than only what did not.
[[nodiscard]] std::string symbolNames(std::vector<std::uint8_t> const& b) {
    auto const sections = readSections(b);
    auto const* symtab  = findSection(sections, ".symtab");
    if (symtab == nullptr || symtab->entSize == 0
        || symtab->link >= sections.size()) {
        return "<no .symtab>";
    }
    auto const& strtab = sections[symtab->link];
    std::string all;
    for (std::uint64_t i = 0; i < symtab->size / symtab->entSize; ++i) {
        all += "[";
        all += readCStr(b, strtab.offset
                               + readU32(b, symtab->offset + i * symtab->entSize));
        all += "]";
    }
    return all;
}

struct Schemas {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> reader;   // the ET_REL vocabulary
    std::shared_ptr<ObjectFormatSchema> writer;   // the ET_EXEC image
};

[[nodiscard]] Schemas loadShippedSchemas() {
    Schemas out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(x86_64) failed";
    } else {
        out.target = std::move(t).value();
    }
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-staticlib");
    if (!r.has_value()) {
        ADD_FAILURE() << "loadShipped(elf64-x86_64-linux-staticlib) failed";
    } else {
        out.reader = std::move(r).value();
    }
    auto w = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    if (!w.has_value()) {
        ADD_FAILURE() << "loadShipped(elf64-x86_64-linux-exec) failed";
    } else {
        out.writer = std::move(w).value();
    }
    return out;
}

[[nodiscard]] std::string errorText(DiagnosticReporter const& reporter) {
    std::string all;
    for (auto const& d : reporter.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        all += d.actual;
        all += "\n";
    }
    return all;
}

// ── The static link, done by hand at the module tier ─────────────────────
//
// A member pulled out of an archive still names symbols other members define.
// `linker::mergeModules` is what supplies them in a real `--resolve-library`
// link; here each remaining `externImports` row is given a DEFINITION of its
// own — an 8-byte `.data` object under the SAME SymbolId, plus its
// `ModuleSymbol` name row so the emitted `.symtab` still carries the name this
// file asserts against.
//
// ★ WHY THIS IS THE RIGHT TIER FOR THIS ROW, AND NOT A SHORTCUT. The row is
// about the RELOCATION — a wire type read out of a foreign member and the slot
// its resolution needs. Pulling the ~2200-member transitive closure of glibc's
// `exit.o` would additionally require R_X86_64_GOTTPOFF and R_X86_64_TPOFF32
// (TLS initial-exec, 1912 sites archive-wide) and R_X86_64_REX_GOTPCRELX,
// which are three OTHER scopes — see this file's test 3 and the row's
// cross-refs. What is measured here is exactly what the row names, on the
// member's real bytes.
[[nodiscard]] std::size_t defineRemainingExterns(AssembledModule& mod) {
    std::size_t defined = 0;
    for (auto const& ext : mod.externImports) {
        // ★ THE KIND FOLLOWS THE MEMBER, NOT THE TEST'S CONVENIENCE. The reader
        // typed each undefined symbol from the member's own symtab and its
        // relocations (`isData` is forced FALSE for anything a PLT-variant reloc
        // calls), so `__call_tls_dtors` and `_IO_cleanup` — called at
        // `.rela.text` 0x294 / 0x28a AND GOT-referenced at 0x1b / 0x123 — arrive
        // here as FUNCTIONS, which is what they are in glibc. Defining them as
        // data would make this test's link a different program from the one the
        // member describes.
        if (ext.isData) {
            AssembledData d;
            d.symbol    = ext.symbol;
            d.section   = DataSectionKind::Data;
            d.bytes.assign(8, std::uint8_t{0});
            d.alignment = Alignment::ofRuntimePow2(8);
            mod.dataItems.push_back(std::move(d));
        } else {
            AssembledFunction f;
            f.symbol = ext.symbol;
            f.bytes.assign(1, std::uint8_t{0xC3});   // `ret`
            mod.functions.push_back(std::move(f));
        }
        ModuleSymbol ms;
        ms.symbol  = ext.symbol;
        ms.name    = ext.mangledName;
        ms.binding = SymbolBinding::Global;
        mod.symbols.push_back(std::move(ms));
        ++defined;
    }
    mod.externImports.clear();
    mod.expectedFuncCount = mod.functions.size();
    return defined;
}

// Every GOT-slot-relative site the module carries, located by the TARGET
// SCHEMA's formula rather than by a wire number — the reader already mapped
// the wire id onto a `RelocationKind` and the walker dispatches on the formula,
// so this is the same question both of them ask.
struct GotSite {
    std::size_t   funcIndex = 0;
    std::uint64_t offset    = 0;
    SymbolId      target{};
    std::int64_t  addend    = 0;
};

[[nodiscard]] std::vector<GotSite> gotSitesOf(AssembledModule const& mod,
                                              TargetSchema const&    target) {
    std::vector<GotSite> out;
    for (std::size_t fi = 0; fi < mod.functions.size(); ++fi) {
        for (auto const& rel : mod.functions[fi].relocations) {
            auto const* tri = target.relocationInfo(rel.kind);
            if (tri == nullptr) continue;
            if (tri->formulaKind != RelocFormulaKind::X86_64GotPcRel) continue;
            out.push_back({fi, rel.offset, rel.target, rel.addend});
        }
    }
    return out;
}

}  // namespace

// ── 1. THE CLOSURE ────────────────────────────────────────────────────────
TEST(GlibcGotPcRelResolution,
     RealExitMemberGotPcRelSitesResolveThroughSlotsTheLinkMints) {
    auto const S = loadShippedSchemas();
    ASSERT_TRUE(S.target && S.reader && S.writer);

    DiagnosticReporter readReporter;
    auto module = elf::readRelocatableObject(dss::test::glibcExitObject(),
                                             *S.target, *S.reader,
                                             readReporter);
    ASSERT_TRUE(module.has_value())
        << "the shipped vocabulary must READ the real glibc `exit.o`. Got:\n"
        << errorText(readReporter);

    auto const sites = gotSitesOf(*module, *S.target);
    ASSERT_EQ(sites.size(), 2u)
        << "the fixture member carries exactly two plain-GOTPCREL sites "
           "(`.rela.text` 0x1b against `__call_tls_dtors` and 0x123 against "
           "`_IO_cleanup`)";
    // Each site by the NAMES the image will carry — its function and its
    // target — recorded before the module is linked: the link inserts its
    // entry trampoline ahead of every function, so a `.text` offset measured
    // on the module would no longer point at the site.
    struct NamedSite {
        std::string   function;
        std::uint64_t offset = 0;
        std::int64_t  addend = 0;
        std::string   target;
    };
    auto const nameOf = [&](SymbolId id) {
        for (auto const& s : module->symbols) {
            if (s.symbol == id) return s.name;
        }
        for (auto const& e : module->externImports) {
            if (e.symbol == id) return e.mangledName;
        }
        return std::string{};
    };
    std::vector<NamedSite> named;
    for (auto const& s : sites) {
        EXPECT_EQ(s.addend, -5)
            << "a real `cmpq $0x0,sym@GOTPCREL(%rip)` site is 5 bytes from the "
               "next instruction (4-byte displacement + 1-byte immediate)";
        named.push_back({nameOf(module->functions[s.funcIndex].symbol), s.offset,
                         s.addend, nameOf(s.target)});
        ASSERT_FALSE(named.back().function.empty());
        ASSERT_FALSE(named.back().target.empty());
    }
    EXPECT_NE(named[0].target, named[1].target) << "two distinct targets";

    std::size_t const definedCount = defineRemainingExterns(*module);
    EXPECT_GT(definedCount, 0u)
        << "the member names undefined symbols other archive members define; "
           "if this is zero the fixture stopped being a real archive member";

    // THROUGH THE DRIVER'S LINK: the entry trampoline imports libc `exit`, so
    // the image is the DYNAMIC writer's — exactly as for every program.
    module->userEntrySymbol = module->functions.front().symbol;
    DiagnosticReporter linkReporter;
    auto const image = linker::link(*module, *S.target, *S.writer, linkReporter);
    ASSERT_FALSE(linkReporter.hasErrors())
        << "the read member must LINK through `linker::link` once its externs "
           "are defined — a GOT-slot-relative relocation is lowered, not refused. "
           "Got:\n"
        << errorText(linkReporter);
    ASSERT_FALSE(image.bytes.empty());

    auto const sections = readSections(image.bytes);
    auto const* text = findSection(sections, ".text");
    ASSERT_NE(text, nullptr);
    std::vector<std::uint64_t> slotVas;
    for (auto const& s : named) {
        auto const fnVa = symbolValue(image.bytes, s.function);
        ASSERT_TRUE(fnVa.has_value())
            << "`.symtab` must name " << s.function << ". It carries: "
            << symbolNames(image.bytes);
        // disp = slotVa + A - P  ⇒  slotVa = disp - A + P
        std::uint64_t const siteVa = *fnVa + s.offset;
        std::uint64_t const siteAt = text->offset + (siteVa - text->addr);
        std::int64_t const  disp   = readI32(image.bytes, siteAt);
        std::uint64_t const slotVa = static_cast<std::uint64_t>(
            disp - s.addend + static_cast<std::int64_t>(siteVa));
        // The slot is a pointer inside the image's writable, file-backed data.
        Section const* home = nullptr;
        for (auto const& sec : sections) {
            if (sec.type == 1u && (sec.flags & 0x3u) == 0x3u
                && slotVa >= sec.addr && slotVa + 8 <= sec.addr + sec.size) {
                home = &sec;
            }
        }
        ASSERT_NE(home, nullptr)
            << s.function << "+" << s.offset << " names " << slotVa
            << ", which lies in no writable PROGBITS section of the image";
        ASSERT_EQ(slotVa % 8u, 0u) << "a misaligned slot";
        std::uint64_t const slotContent =
            readU64(image.bytes, home->offset + (slotVa - home->addr));
        // A non-PIE exec: the slot is a LINK-TIME CONSTANT — it must already
        // hold the target's address.
        auto const symVa = symbolValue(image.bytes, s.target);
        ASSERT_TRUE(symVa.has_value())
            << "the emitted `.symtab` must name the GOT target. It carries: "
            << symbolNames(image.bytes);
        EXPECT_EQ(slotContent, *symVa)
            << "the slot " << s.function << " reads must hold " << s.target
            << "'s resolved address — anything else makes every load through it "
               "read the wrong object";
        EXPECT_NE(slotContent, 0u)
            << "these targets ARE defined in this link, so a zero slot would "
               "mean nothing filled it";
        slotVas.push_back(slotVa);
    }
    EXPECT_NE(slotVas[0], slotVas[1])
        << "two distinct targets ⇒ two slots — one per SYMBOL, not one shared";
}

// ── 2. THE CONTROL — no GOT-slot relocation, no slot ─────────────────────
TEST(GlibcGotPcRelResolution, AModuleWithNoGotSlotRelocationGetsNoSlot) {
    auto const S = loadShippedSchemas();
    ASSERT_TRUE(S.target && S.reader && S.writer);

    DiagnosticReporter readReporter;
    auto module = elf::readRelocatableObject(dss::test::gccLibC164Object(),
                                             *S.target, *S.reader,
                                             readReporter);
    ASSERT_TRUE(module.has_value()) << errorText(readReporter);
    ASSERT_TRUE(gotSitesOf(*module, *S.target).empty())
        << "premise: this control object carries no GOT-slot-relative site";

    (void)defineRemainingExterns(*module);
    DiagnosticReporter lowerReporter;
    AssembledModule lowered;
    EXPECT_TRUE(linker::lowerGotSlotReferences(*module, lowered, *S.target,
                                               *S.writer, lowerReporter))
        << "a module naming no GOT slot must leave the lowering nothing to do — "
           "otherwise test 1's slots say nothing about the relocation, only "
           "about the link";
    EXPECT_FALSE(lowerReporter.hasErrors()) << errorText(lowerReporter);
    EXPECT_TRUE(lowered.dataItems.empty() && lowered.functions.empty())
        << "and `out` untouched";

    module->userEntrySymbol = module->functions.front().symbol;
    DiagnosticReporter linkReporter;
    auto const image = linker::link(*module, *S.target, *S.writer, linkReporter);
    EXPECT_FALSE(linkReporter.hasErrors()) << errorText(linkReporter);
    EXPECT_FALSE(image.bytes.empty());
}

// ── 3. THE GUARD — widening by one relocation did not weaken it ──────────
TEST(GlibcGotPcRelResolution, AStillUndeclaredWireTypeIsStillRefused) {
    auto const S = loadShippedSchemas();
    ASSERT_TRUE(S.target && S.reader);

    bool declaresGotTpOff = false;
    for (auto const& r : S.reader->relocations()) {
        if (r.nativeId == kRX8664GotTpOff || r.pltNativeId == kRX8664GotTpOff) {
            declaresGotTpOff = true;
        }
    }
    ASSERT_FALSE(declaresGotTpOff)
        << "this test pins the refusal of R_X86_64_GOTTPOFF; the shipped format "
           "now declares it, so the pin needs re-aiming rather than silently "
           "asserting something else";

    DiagnosticReporter reporter;
    auto const module = elf::readRelocatableObject(
        dss::test::glibcUselocaleObject(), *S.target, *S.reader, reporter);
    EXPECT_FALSE(module.has_value())
        << "resolving GOTPCREL must not make the reader permissive: a "
           "still-undeclared type stays refused";
    EXPECT_NE(errorText(reporter).find("relocation type "
                                       + std::to_string(kRX8664GotTpOff)),
              std::string::npos)
        << "and the refusal must still NAME the type it could not map. Got:\n"
        << errorText(reporter);
}

// ── 4. THE LINKER-DEFINED GOT BASE IS NOT AN IMPORT ──────────────────────
//
// D-LK-ELF-READER-REFUSES-GOTPCREL-BLOCKS-REAL-GLIBC-MEMBERS.
//
// `_GLOBAL_OFFSET_TABLE_` rides in the real member's symtab as an
// UNREFERENCED `NOTYPE GLOBAL UND` entry — the assembler's GOT bookkeeping,
// not a reference. ✔MEASURED with `readelf -rW` on this exact member: it is
// symtab index 3 and NONE of the 33 `.rela.text` entries name it.
//
// ★ THE ASSERTION IS ON THE NAME, NOT ON A COUNT, and that is the point. A
// count pins "the reader dropped ONE row" and would stay green if the reader
// dropped a DIFFERENT row and minted this one. The name is the property the
// linker's correctness depends on: minting an import here would put a
// `.dynsym` UND row in the image for a symbol NO library exports
// (✔MEASURED: `nm -D libc.so.6` has zero hits), and the image would fail to
// LOAD. The count is asserted too, but as the SECOND half — every other
// undefined symbol the member names must still arrive.
TEST(GlibcGotPcRelResolution, TheLinkerDefinedGotBaseIsNotAnExternImport) {
    auto const S = loadShippedSchemas();
    ASSERT_TRUE(S.target && S.reader);

    DiagnosticReporter reporter;
    auto const module = elf::readRelocatableObject(
        dss::test::glibcExitObject(), *S.target, *S.reader, reporter);
    ASSERT_TRUE(module.has_value()) << errorText(reporter);

    for (auto const& ext : module->externImports) {
        EXPECT_NE(ext.mangledName, "_GLOBAL_OFFSET_TABLE_")
            << "the GOT base is defined by the LINKER — no object defines it "
               "and no library exports it, so an import row for it is a "
               "`.dynsym` entry nothing can ever resolve";
    }

    // The other NINE undefined symbols this member names are REAL imports and
    // must all survive — a reader that dropped them too would satisfy the
    // assertion above while destroying the link. ✔MEASURED with
    // `readelf -sW exit.o` over its UND rows: exactly these nine, plus the GOT
    // base the assertion above excludes.
    std::vector<std::string> names;
    names.reserve(module->externImports.size());
    for (auto const& ext : module->externImports) names.push_back(ext.mangledName);
    std::sort(names.begin(), names.end());
    std::vector<std::string> expected{
        "_IO_cleanup",             "__call_tls_dtors",
        "__exit_funcs",            "__exit_funcs_lock",
        "__lll_lock_wait_private", "__lll_lock_wake_private",
        "__new_exitfn_called",     "_exit",
        "free"};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(names, expected)
        << "the member's REAL undefined references, unchanged";
}
