// P68 round 11 — GOT-SLOT-RELATIVE REFERENCES, LOWERED TO SLOTS THE LINK MINTS
// (D-LK-ELF-READER-REFUSES-GOTPCREL-BLOCKS-REAL-GLIBC-MEMBERS — the x86_64
// GOTPCREL / GOTPCRELX / REX_GOTPCRELX half;
// D-LK-ELF-AARCH64-EXEC-REFUSES-GOT-RELOCATIONS — the aarch64 ADR_GOT_PAGE +
// LD64_GOT_LO12_NC half).
//
// A default-built (PIE) gcc object reaches an extern datum, and takes an extern
// function's address, through a GOT slot; so does DSS's own aarch64 staticlib.
// ✔MEASURED 2026-09-24 before this change: every such member was refused by a
// DSS image link — at the kind unifier (the exec documents declared no GOT
// kind), at READ (x86 type 42 undeclared), or, with the kinds declared, by the
// DYNAMIC writer every ELF image takes. `lowerGotSlotReferences` now mints each
// slot as a relocation-bearing data item and rewrites the reference into its
// direct twin; the pins below read what that produces.
//
// ── WHAT EACH TEST HERE IS FOR ─────────────────────────────────────────
//  A. The lowering itself, on modules built to isolate one rule each: where
//     the addend goes (x86: on the reference; aarch64: into the slot — AAELF64's
//     GDAT(S+A)), the twin each formula becomes, a relocatable format left
//     alone, a weak symbol resolved to nothing, a missing twin, a GOT relocation
//     in a data item.
//  B. The REAL gcc members (`got_member_objects.inc`), linked through
//     `linker::link` with a program that defines what they name: every site's
//     slot holds its symbol's address in an exec, is covered by a RELATIVE row
//     in a PIE, and by a row against the symbol for a preemptible definition in
//     a shared object — the three fills of one slot, per image kind.
//  C. A LIBRARY DATUM (`library_datum_member_objects.inc`, libc's `stdout`):
//     reached through the GOT it links, and the loader fills the slot the link
//     minted; named DIRECTLY — gcc's default x86_64 PIE code, which assumes a
//     copy relocation — it is refused by name, because DSS makes no copy
//     relocation and every image writer used to bind such a reference to the
//     datum's loader-filled slot, a silent wrong answer (✔MEASURED 2026-09-24:
//     exit 2 where gcc's link of the same archive exits 42). The GOT lowering
//     is what made that reachable from members that used to be refused at read.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/fresh_symbol_ids.hpp"
#include "link/got_slots.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include "got_member_objects.inc"
#include "library_datum_member_objects.inc"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

// ── schemas ────────────────────────────────────────────────────────────────

[[nodiscard]] std::shared_ptr<TargetSchema const> target(char const* name) {
    auto t = TargetSchema::loadShipped(name);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(" << name << ") failed";
        return nullptr;
    }
    return std::move(t).value();
}
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> format(char const* name) {
    auto f = ObjectFormatSchema::loadShipped(name);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << name << ") failed";
        return nullptr;
    }
    return std::move(f).value();
}
[[nodiscard]] RelocationKind kindNamed(TargetSchema const& t, char const* name) {
    auto const* r = t.relocationByName(name);
    EXPECT_NE(r, nullptr) << name;
    return r != nullptr ? r->kind : RelocationKind{};
}
[[nodiscard]] std::string errorText(DiagnosticReporter const& rep) {
    std::string all;
    for (auto const& d : rep.all()) {
        if (d.severity == DiagnosticSeverity::Error) all += d.actual + "\n";
    }
    return all;
}

// ── A module with ONE function whose relocations are the subject ─────────────

[[nodiscard]] AssembledModule moduleWith(std::vector<Relocation> relocs,
                                         bool datumIsAnImport = false) {
    AssembledModule m;
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes.assign(64, 0);
    fn.relocations = std::move(relocs);
    m.functions.push_back(std::move(fn));
    if (datumIsAnImport) {
        ExternImport ext;
        ext.symbol      = SymbolId{2};
        ext.mangledName = "datum";
        ext.libraryPath = "libx.so";
        ext.isData      = true;
        m.externImports.push_back(ext);
    } else {
        AssembledData d;
        d.symbol    = SymbolId{2};
        d.section   = DataSectionKind::Data;
        d.bytes.assign(8, 0);
        d.alignment = Alignment::ofRuntimePow2(8);
        m.dataItems.push_back(d);
    }
    return m;
}

[[nodiscard]] AssembledData const* itemNamed(AssembledModule const& m, SymbolId id) {
    for (auto const& d : m.dataItems) {
        if (d.symbol == id) return &d;
    }
    return nullptr;
}

// ── ELF64 image readers ───────────────────────────────────────────────────────

[[nodiscard]] std::uint64_t rd(std::vector<std::uint8_t> const& b, std::uint64_t off,
                               int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(b.at(off + i)) << (8 * i);
    return v;
}
[[nodiscard]] std::string cstr(std::vector<std::uint8_t> const& b, std::uint64_t off) {
    std::string s;
    while (off < b.size() && b[off] != 0) s.push_back(static_cast<char>(b[off++]));
    return s;
}
struct Section {
    std::string   name;
    std::uint32_t type = 0, link = 0;
    std::uint64_t flags = 0, addr = 0, offset = 0, size = 0, entSize = 0;
};
[[nodiscard]] std::vector<Section> sectionsOf(std::vector<std::uint8_t> const& b) {
    std::vector<Section> out;
    std::uint64_t const shoff = rd(b, 40, 8);
    auto const shnum = static_cast<std::uint16_t>(rd(b, 60, 2));
    auto const shstrndx = static_cast<std::uint16_t>(rd(b, 62, 2));
    std::uint64_t const shstr = rd(b, shoff + shstrndx * 64ull + 24, 8);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const h = shoff + i * 64ull;
        out.push_back({cstr(b, shstr + rd(b, h, 4)), static_cast<std::uint32_t>(rd(b, h + 4, 4)),
                       static_cast<std::uint32_t>(rd(b, h + 40, 4)), rd(b, h + 8, 8),
                       rd(b, h + 16, 8), rd(b, h + 24, 8), rd(b, h + 32, 8), rd(b, h + 56, 8)});
    }
    return out;
}
[[nodiscard]] Section const* section(std::vector<Section> const& v, std::string const& n) {
    for (auto const& s : v) {
        if (s.name == n) return &s;
    }
    return nullptr;
}
// `st_value` of a named symbol of the given symbol table (`.symtab` / `.dynsym`).
[[nodiscard]] std::optional<std::uint64_t> symbolValue(std::vector<std::uint8_t> const& b,
                                                       std::string const& want,
                                                       char const* table = ".symtab") {
    auto const secs = sectionsOf(b);
    auto const* st = section(secs, table);
    if (st == nullptr || st->entSize == 0 || st->link >= secs.size()) return std::nullopt;
    for (std::uint64_t i = 0; i < st->size / st->entSize; ++i) {
        std::uint64_t const e = st->offset + i * st->entSize;
        if (cstr(b, secs[st->link].offset + rd(b, e, 4)) == want) return rd(b, e + 8, 8);
    }
    return std::nullopt;
}
// The file offset of a VA, through the section that holds it.
[[nodiscard]] std::optional<std::uint64_t> fileOffsetOf(std::vector<Section> const& secs,
                                                        std::uint64_t va) {
    for (auto const& s : secs) {
        if (s.type == 1u && (s.flags & 0x2u) != 0 && va >= s.addr && va < s.addr + s.size) {
            return s.offset + (va - s.addr);
        }
    }
    return std::nullopt;
}
struct Rela {
    std::uint64_t offset = 0;
    std::uint32_t type = 0;
    std::string   symbol;   // "" for a symbol-less row (RELATIVE)
    std::int64_t  addend = 0;
};
[[nodiscard]] std::vector<Rela> relaDyn(std::vector<std::uint8_t> const& b) {
    std::vector<Rela> out;
    auto const secs = sectionsOf(b);
    auto const* rela = section(secs, ".rela.dyn");
    auto const* dynsym = section(secs, ".dynsym");
    if (rela == nullptr) return out;
    for (std::uint64_t o = rela->offset; o + 24 <= rela->offset + rela->size; o += 24) {
        std::uint64_t const info = rd(b, o + 8, 8);
        std::uint32_t const symIdx = static_cast<std::uint32_t>(info >> 32);
        std::string name;
        if (symIdx != 0 && dynsym != nullptr && dynsym->link < secs.size()) {
            name = cstr(b, secs[dynsym->link].offset
                               + rd(b, dynsym->offset + symIdx * 24ull, 4));
        }
        out.push_back({rd(b, o, 8), static_cast<std::uint32_t>(info & 0xFFFFFFFFu), name,
                       static_cast<std::int64_t>(rd(b, o + 16, 8))});
    }
    return out;
}

// ── The program the real members are linked into ─────────────────────────────

// The program's two data definitions carry recognizable bytes, so a test can
// find each one — and so its ADDRESS — in the image: an image's `.symtab` names
// its functions but is not relied on for data here.
constexpr std::uint8_t kValueMarker[8] = {0x11, 0xD5, 0x5D, 0xA7, 0xA1, 0x0D, 0xF0, 0x0D};
constexpr std::uint8_t kTableMarker[16] = {0x01, 0x7E, 0xC3, 0x55, 0x02, 0x7E, 0xC3, 0x55,
                                           0x19, 0x7E, 0xC3, 0x55, 0x04, 0x7E, 0xC3, 0x55};

// Its own compilation unit: the three symbols the members name, and `main`.
[[nodiscard]] AssembledModule programModule(bool arm64) {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    std::vector<std::uint8_t> const ret =
        arm64 ? std::vector<std::uint8_t>{0xC0, 0x03, 0x5F, 0xD6}
              : std::vector<std::uint8_t>{0xC3};
    AssembledFunction mainFn;
    mainFn.symbol = SymbolId{1};
    mainFn.bytes  = ret;
    AssembledFunction twice;
    twice.symbol = SymbolId{2};
    twice.bytes  = ret;
    m.functions = {mainFn, twice};
    m.expectedFuncCount = 2;
    AssembledData value;
    value.symbol    = SymbolId{3};
    value.section   = DataSectionKind::Data;
    value.bytes.assign(std::begin(kValueMarker), std::end(kValueMarker));
    value.alignment = Alignment::ofRuntimePow2(8);
    AssembledData table;
    table.symbol    = SymbolId{4};
    table.section   = DataSectionKind::Data;
    table.bytes.assign(std::begin(kTableMarker), std::end(kTableMarker));
    table.alignment = Alignment::ofRuntimePow2(8);
    m.dataItems = {value, table};
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{2}, "dss_app_twice", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{3}, "dss_app_value", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{4}, "dss_app_table", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    return m;
}

// The address of one of the three definitions the members name: a function by
// `.symtab`, a datum by its marker bytes (found in a file-backed section).
[[nodiscard]] std::optional<std::uint64_t> addressOf(std::vector<std::uint8_t> const& b,
                                                     std::string const& name) {
    std::vector<std::uint8_t> marker;
    if (name == "dss_app_value") marker.assign(std::begin(kValueMarker), std::end(kValueMarker));
    if (name == "dss_app_table") marker.assign(std::begin(kTableMarker), std::end(kTableMarker));
    if (marker.empty()) return symbolValue(b, name);
    for (auto const& s : sectionsOf(b)) {
        if (s.type != 1u || (s.flags & 0x2u) == 0) continue;
        for (std::uint64_t o = s.offset; o + marker.size() <= s.offset + s.size; ++o) {
            if (std::equal(marker.begin(), marker.end(), b.begin() + static_cast<std::ptrdiff_t>(o))) {
                return s.addr + (o - s.offset);
            }
        }
    }
    return std::nullopt;
}

// One GOT-slot-relative site of a member, by the NAMES the image will carry.
struct GotSite {
    std::string      function;
    std::uint64_t    offset = 0;
    std::int64_t     addend = 0;
    std::string      target;
    RelocFormulaKind formula = RelocFormulaKind::Linear;
};

struct Members {
    std::vector<AssembledModule> modules;
    std::vector<GotSite>         sites;
};

[[nodiscard]] Members readMembers(TargetSchema const& t, ObjectFormatSchema const& reader,
                                  std::vector<std::vector<std::uint8_t>> const& objects) {
    Members out;
    std::uint32_t cu = 2;
    for (auto const& bytes : objects) {
        DiagnosticReporter rep;
        auto m = elf::readRelocatableObject(bytes, t, reader, rep, CompilationUnitId{cu++});
        if (!m.has_value()) {
            ADD_FAILURE() << "a gcc member failed to READ:\n" << errorText(rep);
            continue;
        }
        auto const nameOf = [&](SymbolId id) {
            for (auto const& s : m->symbols) {
                if (s.symbol == id) return s.name;
            }
            for (auto const& e : m->externImports) {
                if (e.symbol == id) return e.mangledName;
            }
            return std::string{};
        };
        for (auto const& fn : m->functions) {
            for (auto const& rel : fn.relocations) {
                auto const* tri = t.relocationInfo(rel.kind);
                if (tri == nullptr || !relocFormulaFacts(tri->formulaKind).isGotSlotRelative)
                    continue;
                out.sites.push_back({nameOf(fn.symbol), rel.offset, rel.addend,
                                     nameOf(rel.target), tri->formulaKind});
            }
        }
        out.modules.push_back(std::move(*m));
    }
    return out;
}

struct Linked {
    std::vector<std::uint8_t> bytes;
    std::string               errors;
};

[[nodiscard]] Linked linkWithProgram(Members members, bool arm64, TargetSchema const& t,
                                     ObjectFormatSchema const& image) {
    std::vector<AssembledModule> mods;
    mods.push_back(programModule(arm64));
    for (auto& m : members.modules) mods.push_back(std::move(m));
    DiagnosticReporter rep;
    auto linked = linker::link(std::span<AssembledModule const>{mods.data(), mods.size()},
                               t, image, rep);
    return {rep.hasErrors() ? std::vector<std::uint8_t>{} : std::move(linked.bytes),
            errorText(rep)};
}

// The slot a site names, read back off the patched instruction(s).
[[nodiscard]] std::optional<std::uint64_t> slotNamedBy(std::vector<std::uint8_t> const& b,
                                                       GotSite const& s,
                                                       std::uint64_t pairLdrOffset = 0) {
    auto const secs = sectionsOf(b);
    auto const* text = section(secs, ".text");
    auto const fnVa = symbolValue(b, s.function);
    if (text == nullptr || !fnVa.has_value()) return std::nullopt;
    std::uint64_t const siteVa = *fnVa + s.offset;
    std::uint64_t const at = text->offset + (siteVa - text->addr);
    if (s.formula == RelocFormulaKind::X86_64GotPcRel) {
        // disp = slot + A - P  ⇒  slot = disp - A + P
        auto const disp = static_cast<std::int32_t>(rd(b, at, 4));
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(disp) - s.addend
                                          + static_cast<std::int64_t>(siteVa));
    }
    // aarch64: the ADRP at this site, its LDR at `pairLdrOffset` in the function.
    auto const adrp = static_cast<std::uint32_t>(rd(b, at, 4));
    std::uint64_t const ldrAt = text->offset + (*fnVa + pairLdrOffset - text->addr);
    auto const ldr = static_cast<std::uint32_t>(rd(b, ldrAt, 4));
    std::int64_t pages = ((static_cast<std::int64_t>(adrp >> 5) & 0x7FFFF) << 2)
                       | ((adrp >> 29) & 0x3);
    if ((pages & (std::int64_t{1} << 20)) != 0) pages -= std::int64_t{1} << 21;
    std::uint64_t const page = (siteVa & ~std::uint64_t{0xFFF})
                             + static_cast<std::uint64_t>(pages * 4096);
    return page + (((ldr >> 10) & 0xFFFu) << 3);
}

// For each aarch64 ADRP site, the offset of the LD64 half naming the same
// symbol in the same function (the pairs are emitted adjacent by gcc, but the
// pin matches them by symbol rather than by distance).
[[nodiscard]] std::map<std::size_t, std::uint64_t> pairUp(std::vector<GotSite> const& sites) {
    std::map<std::size_t, std::uint64_t> ldrOf;
    for (std::size_t i = 0; i < sites.size(); ++i) {
        if (sites[i].formula != RelocFormulaKind::Aarch64AdrGotPage) continue;
        for (std::size_t j = i + 1; j < sites.size(); ++j) {
            if (sites[j].formula == RelocFormulaKind::Aarch64Ld64GotLo12
                && sites[j].function == sites[i].function
                && sites[j].target == sites[i].target) {
                ldrOf[i] = sites[j].offset;
                break;
            }
        }
    }
    return ldrOf;
}

// ── Section C: a library datum ───────────────────────────────────────────────

[[nodiscard]] bool reported(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.severity == DiagnosticSeverity::Error && d.code == code) return true;
    }
    return false;
}

// `main`, and the one program function the mixed member takes the address of.
[[nodiscard]] AssembledModule appFnProgram(bool arm64) {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    std::vector<std::uint8_t> const ret =
        arm64 ? std::vector<std::uint8_t>{0xC0, 0x03, 0x5F, 0xD6}
              : std::vector<std::uint8_t>{0xC3};
    AssembledFunction mainFn;
    mainFn.symbol = SymbolId{1};
    mainFn.bytes  = ret;
    AssembledFunction appFn;
    appFn.symbol = SymbolId{2};
    appFn.bytes  = ret;
    m.functions = {mainFn, appFn};
    m.expectedFuncCount = 2;
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{2}, "app_fn", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    return m;
}

// What the compile pipeline's archive-member binder does with a member's
// undefined `stdout`: the platform corpus names its library. The reader leaves
// `readThroughSlot` false, as it does on every row it mints.
void bindStdoutToLibc(Members& members) {
    for (auto& m : members.modules) {
        for (auto& e : m.externImports) {
            if (e.mangledName == "stdout") e.libraryPath = "libc.so.6";
        }
    }
}

[[nodiscard]] std::vector<std::uint8_t> linkWithAppFn(Members members, bool arm64,
                                                      TargetSchema const& t,
                                                      ObjectFormatSchema const& image,
                                                      DiagnosticReporter& rep) {
    std::vector<AssembledModule> mods;
    mods.push_back(appFnProgram(arm64));
    for (auto& m : members.modules) mods.push_back(std::move(m));
    auto linked = linker::link(std::span<AssembledModule const>{mods.data(), mods.size()},
                               t, image, rep);
    return rep.hasErrors() ? std::vector<std::uint8_t>{} : std::move(linked.bytes);
}

}  // namespace

// ══ A. THE LOWERING ══════════════════════════════════════════════════════════

TEST(GotSlotLowering, X86KeepsTheAddendOnTheReferenceAndSharesOneSlot) {
    auto const t = target("x86_64");
    auto const f = format("elf64-x86_64-linux-exec");
    ASSERT_TRUE(t && f);
    RelocationKind const got = kindNamed(*t, "gotpcrel32");
    RelocationKind const gotx = kindNamed(*t, "rex_gotpcrelx32");
    // Two sites naming ONE symbol with DIFFERENT addends — a `mov` (-4) and a
    // `cmpq $0` (-5): x86-64's G + GOT + A - P keeps A on the reference, so
    // both read the ONE slot holding `datum`.
    AssembledModule in = moduleWith({Relocation{3, SymbolId{2}, got, -4},
                                     Relocation{13, SymbolId{2}, gotx, -5}});
    AssembledModule out;
    DiagnosticReporter rep;
    ASSERT_FALSE(linker::lowerGotSlotReferences(in, out, *t, *f, rep)) << "lowered";
    ASSERT_FALSE(rep.hasErrors()) << errorText(rep);
    ASSERT_EQ(out.dataItems.size(), 2u) << "the datum and ONE slot";
    auto const& r0 = out.functions[0].relocations[0];
    auto const& r1 = out.functions[0].relocations[1];
    EXPECT_EQ(r0.target, r1.target) << "one slot for one symbol";
    EXPECT_EQ(r0.kind, kindNamed(*t, "pcrel32")) << "the direct twin: pc-relative, 4 bytes, bias 0";
    EXPECT_EQ(r1.kind, r0.kind) << "GOTPCRELX lowers exactly like GOTPCREL — no relaxation";
    EXPECT_EQ(r0.addend, -4);
    EXPECT_EQ(r1.addend, -5) << "x86's addend is the displacement's own bias — kept";
    auto const* slot = itemNamed(out, r0.target);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->section, DataSectionKind::RelRoConst);
    EXPECT_EQ(slot->bytes.size(), 8u);
    ASSERT_EQ(slot->relocations.size(), 1u);
    EXPECT_EQ(slot->relocations[0].target, SymbolId{2});
    EXPECT_EQ(slot->relocations[0].kind, kindNamed(*t, "abs64"));
    EXPECT_EQ(slot->relocations[0].addend, 0) << "the slot holds the symbol itself";
}

TEST(GotSlotLowering, Aarch64MovesTheAddendIntoTheSlotAndMakesItsOwnSlot) {
    // AAELF64 writes ADR_GOT_PAGE / LD64_GOT_LO12_NC over GDAT(S+A): `datum`
    // and `datum+4` are two slots, and the reference addresses its slot with no
    // addend of its own. (GNU ld 2.42 drops the addend and lld refuses the
    // misaligned load — ✔MEASURED 2026-09-24 — the document decides.)
    auto const t = target("arm64");
    auto const f = format("elf64-aarch64-linux-exec");
    ASSERT_TRUE(t && f);
    RelocationKind const page = kindNamed(*t, "adr_got_page");
    RelocationKind const lo12 = kindNamed(*t, "ld64_got_lo12");
    AssembledModule in = moduleWith({Relocation{0, SymbolId{2}, page, 0},
                                     Relocation{4, SymbolId{2}, lo12, 0},
                                     Relocation{8, SymbolId{2}, page, 4},
                                     Relocation{12, SymbolId{2}, lo12, 4}});
    AssembledModule out;
    DiagnosticReporter rep;
    ASSERT_FALSE(linker::lowerGotSlotReferences(in, out, *t, *f, rep));
    ASSERT_FALSE(rep.hasErrors()) << errorText(rep);
    auto const& rs = out.functions[0].relocations;
    EXPECT_EQ(rs[0].target, rs[1].target) << "the pair names ONE slot";
    EXPECT_EQ(rs[2].target, rs[3].target);
    EXPECT_NE(rs[0].target, rs[2].target) << "datum and datum+4 are two slots";
    EXPECT_EQ(rs[0].kind, kindNamed(*t, "adr_prel_pg_hi21"));
    EXPECT_EQ(rs[1].kind, kindNamed(*t, "ldst64_abs_lo12_nc")) << "the 64-bit LDR's scale-3 field";
    for (auto const& r : rs) EXPECT_EQ(r.addend, 0) << "the addend moved into the slot";
    auto const* slot4 = itemNamed(out, rs[2].target);
    ASSERT_NE(slot4, nullptr);
    ASSERT_EQ(slot4->relocations.size(), 1u);
    EXPECT_EQ(slot4->relocations[0].target, SymbolId{2});
    EXPECT_EQ(slot4->relocations[0].addend, 4) << "the slot HOLDS datum+4";
}

TEST(GotSlotLowering, ARelocatableFormatKeepsItsGotRelocations) {
    // DSS's own aarch64 staticlib WRITES this pair (`externAddrBinding: got`);
    // the final linker owns its slot.
    auto const t = target("arm64");
    auto const f = format("elf64-aarch64-linux-staticlib");
    ASSERT_TRUE(t && f);
    AssembledModule in = moduleWith({Relocation{0, SymbolId{2}, kindNamed(*t, "adr_got_page"), 0},
                                     Relocation{4, SymbolId{2}, kindNamed(*t, "ld64_got_lo12"), 0}});
    AssembledModule out;
    DiagnosticReporter rep;
    EXPECT_TRUE(linker::lowerGotSlotReferences(in, out, *t, *f, rep)) << "nothing to do";
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(out.functions.empty() && out.dataItems.empty()) << "out untouched";
}

TEST(GotSlotLowering, AWeakSymbolResolvedToNothingGetsASlotHoldingZero) {
    auto const t = target("x86_64");
    auto const f = format("elf64-x86_64-linux-exec");
    ASSERT_TRUE(t && f);
    // `cmpq $0x0, w@GOTPCREL(%rip)` — the glibc idiom — against a weak symbol
    // the link bound to NOTHING: the slot must hold 0 (S is 0), not the
    // address of the null slot the reference gate gave the symbol.
    AssembledModule in = moduleWith({Relocation{3, SymbolId{2}, kindNamed(*t, "gotpcrel32"), -5}});
    std::vector<SymbolId> const nothing{SymbolId{2}};
    AssembledModule out;
    DiagnosticReporter rep;
    ASSERT_FALSE(linker::lowerGotSlotReferences(in, out, *t, *f, rep, nothing));
    ASSERT_FALSE(rep.hasErrors()) << errorText(rep);
    auto const* slot = itemNamed(out, out.functions[0].relocations[0].target);
    ASSERT_NE(slot, nullptr);
    EXPECT_TRUE(slot->relocations.empty()) << "no fixup: the slot holds 0";
    EXPECT_EQ(slot->bytes, std::vector<std::uint8_t>(8, 0));
    EXPECT_EQ(slot->section, DataSectionKind::Rodata);

    // The same on aarch64 with an ADDEND would be a slot holding the bare
    // number A — no compiler writes it and the references disagree: refused.
    auto const ta = target("arm64");
    auto const fa = format("elf64-aarch64-linux-exec");
    ASSERT_TRUE(ta && fa);
    AssembledModule ina = moduleWith({Relocation{0, SymbolId{2}, kindNamed(*ta, "adr_got_page"), 8}});
    AssembledModule outa;
    DiagnosticReporter repa;
    (void)linker::lowerGotSlotReferences(ina, outa, *ta, *fa, repa, nothing);
    EXPECT_TRUE(repa.hasErrors()) << "a nothing-plus-addend slot is refused";
}

// ── The twin is DECLARED (`gotSlotTwin`, P68 round 11) and proved at load ──
//
// A GOT row names the direct row an image link lowers it into, and the loader
// proves that row applies the same arithmetic to the slot. It used to be FOUND
// by a search over the target's rows, which could not tell two rows with one
// arithmetic apart — x86_64's `rel32` (a branch) and `riprel32` (a memory
// operand) are both Linear, pc-relative, 4 bytes, bias −4 — and Mach-O's GOT
// rows carry exactly that bias, so the search would have had to pick.

namespace {

// A minimal x86_64-shaped target: `rows` spliced into a relocation table that
// already declares `abs64`, `pcrel32`, `rel32` and `riprel32`.
[[nodiscard]] LoadResult<std::shared_ptr<TargetSchema>>
targetWithGotRows(std::string const& rows) {
    return TargetSchema::loadFromText(std::string{R"({
      "dssTargetVersion": 1,
      "target": {"name":"x86_64-got-twins"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ],
      "relocations": [
        { "name":"rel32", "kind":1, "formula":"linear", "pcRelative":true, "addendBias":-4, "widthBytes":4 },
        { "name":"abs64", "kind":2, "formula":"linear", "pcRelative":false, "addendBias":0, "widthBytes":8 },
        { "name":"pcrel32", "kind":5, "formula":"linear", "pcRelative":true, "addendBias":0, "widthBytes":4 },
        { "name":"riprel32", "kind":8, "formula":"linear", "pcRelative":true, "addendBias":-4, "widthBytes":4 })"}
        + rows + R"(
      ]
    })");
}

[[nodiscard]] std::string loadErrors(LoadResult<std::shared_ptr<TargetSchema>> const& r) {
    std::string all;
    if (!r.has_value()) {
        for (auto const& d : r.error()) all += d.message + "\n";
    }
    return all;
}

}  // namespace

TEST(GotSlotLowering, AGotRowMustNameATwinThatHasItsArithmetic) {
    // (a) No twin: refused at LOAD, naming the row and the key.
    auto const none = targetWithGotRows(
        R"(, { "name":"gotpcrel32", "kind":7, "formula":"x86_64_gotpcrel" })");
    ASSERT_FALSE(none.has_value()) << "a GOT row naming no twin must not load";
    EXPECT_NE(loadErrors(none).find("gotpcrel32"), std::string::npos) << loadErrors(none);
    EXPECT_NE(loadErrors(none).find("gotSlotTwin"), std::string::npos) << loadErrors(none);
    // The ABSENT twin is its own refusal, not an empty name looked up and missed:
    // it says what the row owes rather than that '' is undeclared.
    EXPECT_NE(loadErrors(none).find("must name the direct row"), std::string::npos) << loadErrors(none);
    EXPECT_EQ(loadErrors(none).find("does not declare"), std::string::npos) << loadErrors(none);

    // (b) A twin the target does not declare.
    auto const missing = targetWithGotRows(
        R"(, { "name":"gotpcrel32", "kind":7, "formula":"x86_64_gotpcrel", "gotSlotTwin":"nope" })");
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(loadErrors(missing).find("'nope'"), std::string::npos) << loadErrors(missing);
    EXPECT_NE(loadErrors(missing).find("does not declare"), std::string::npos) << loadErrors(missing);

    // (c) A twin with OTHER arithmetic: a bias-0 GOT row naming a bias −4 row.
    auto const wrongBias = targetWithGotRows(
        R"(, { "name":"gotpcrel32", "kind":7, "formula":"x86_64_gotpcrel", "gotSlotTwin":"riprel32" })");
    ASSERT_FALSE(wrongBias.has_value()) << "the twin must apply the GOT row's own arithmetic";
    EXPECT_NE(loadErrors(wrongBias).find("bias 0"), std::string::npos) << loadErrors(wrongBias);
    EXPECT_NE(loadErrors(wrongBias).find("bias -4"), std::string::npos) << loadErrors(wrongBias);

    // (d) A row that addresses no slot names no twin.
    auto const stray = targetWithGotRows(
        R"(, { "name":"abs32", "kind":3, "formula":"linear", "pcRelative":false, "addendBias":0, "widthBytes":4, "gotSlotTwin":"pcrel32" })");
    ASSERT_FALSE(stray.has_value()) << "a twin nothing reads is refused";
    EXPECT_NE(loadErrors(stray).find("abs32"), std::string::npos) << loadErrors(stray);

    // (e) A twin that addresses a slot itself.
    auto const gotTwin = targetWithGotRows(
        R"(, { "name":"a", "kind":7, "formula":"x86_64_gotpcrel", "gotSlotTwin":"b" },
             { "name":"b", "kind":9, "formula":"x86_64_gotpcrel", "gotSlotTwin":"pcrel32" })");
    ASSERT_FALSE(gotTwin.has_value());
    EXPECT_NE(loadErrors(gotTwin).find("DIRECT"), std::string::npos) << loadErrors(gotTwin);

    // CONTROL: the declaration done right loads.
    auto const good = targetWithGotRows(
        R"(, { "name":"gotpcrel32", "kind":7, "formula":"x86_64_gotpcrel", "gotSlotTwin":"pcrel32" })");
    EXPECT_TRUE(good.has_value()) << loadErrors(good);
}

TEST(GotSlotLowering, TwoRowsWithOneArithmeticAreNeverAChoice) {
    // Mach-O's GOT displacement carries bias −4 (it measures from the END of
    // the field). Two direct rows share that arithmetic; the declaration
    // decides, and the lowering follows it — whichever it names.
    auto const f = format("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(f);
    for (char const* twin : {"riprel32", "rel32"}) {
        auto const t = targetWithGotRows(
            std::string{R"(, { "name":"gotriprel32", "kind":11, "formula":"x86_64_gotpcrel", "addendBias":-4, "gotSlotTwin":")"}
            + twin + R"(" })");
        ASSERT_TRUE(t.has_value()) << loadErrors(t);
        AssembledModule in = moduleWith({Relocation{3, SymbolId{2}, RelocationKind{11}, -1}});
        AssembledModule out;
        DiagnosticReporter rep;
        ASSERT_FALSE(linker::lowerGotSlotReferences(in, out, **t, *f, rep)) << errorText(rep);
        ASSERT_FALSE(rep.hasErrors()) << errorText(rep);
        auto const& r = out.functions[0].relocations[0];
        EXPECT_EQ(r.kind, (*t)->relocationByName(twin)->kind)
            << "the reference becomes the DECLARED twin, '" << twin << "'";
        EXPECT_EQ(r.addend, -1) << "x86's addend stays on the reference";
    }
}

TEST(GotSlotLowering, APlainFieldFormulaMayCarryABiasAndAnInstructionFormulaMayNot) {
    // x86-64's GOT displacement is four plain bytes — Mach-O measures it from
    // the field's end, so its row carries bias −4 as `riprel32` does.
    auto const x86 = targetWithGotRows(
        R"(, { "name":"gotriprel32", "kind":11, "formula":"x86_64_gotpcrel", "addendBias":-4, "gotSlotTwin":"riprel32" })");
    EXPECT_TRUE(x86.has_value()) << loadErrors(x86);
    // An AArch64 GOT formula patches an instruction word and encodes its own
    // arithmetic: a bias declared there would be applied twice.
    auto const a64 = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"a64-got-bias"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ],
      "relocations": [
        { "name":"page", "kind":2, "formula":"aarch64_adr_prel_pg_hi21" },
        { "name":"gotpage", "kind":7, "formula":"aarch64_adr_got_page", "addendBias":-4, "gotSlotTwin":"page" }
      ]
    })");
    ASSERT_FALSE(a64.has_value());
    EXPECT_NE(loadErrors(a64).find("addendBias"), std::string::npos) << loadErrors(a64);
}

TEST(GotSlotLowering, AFreshSlotIdNeverCollidesWithAnImportsAddressSlot) {
    // A preemption reference in a `.so` names an import's ADDRESS through a
    // second id of its own (`ExternImport::addressSlotSymbol`), and the scan
    // every minting pass asks for the taken ids did not read it. ✔MEASURED
    // 2026-09-24: the first aarch64 `.so` this lowering ran on was refused —
    // "symbol #11 ... is declared more than once" — its minted slot having
    // taken the address slot's id.
    auto const t = target("arm64");
    auto const f = format("elf64-aarch64-linux-dyn");
    ASSERT_TRUE(t && f);
    AssembledModule in = moduleWith({Relocation{0, SymbolId{2}, kindNamed(*t, "adr_got_page"), 0},
                                     Relocation{4, SymbolId{2}, kindNamed(*t, "ld64_got_lo12"), 0}},
                                    /*datumIsAnImport=*/true);
    in.externImports.front().addressSlotSymbol = SymbolId{40};   // the largest id in the module
    EXPECT_EQ(linker::maxExistingSymbolIdV(in), 40u)
        << "the address slot's id is TAKEN";
    AssembledModule out;
    DiagnosticReporter rep;
    ASSERT_FALSE(linker::lowerGotSlotReferences(in, out, *t, *f, rep));
    ASSERT_FALSE(rep.hasErrors()) << errorText(rep);
    EXPECT_GT(out.functions[0].relocations[0].target.v, 40u)
        << "the slot is minted PAST every taken id, the address slot's included";
}

TEST(GotSlotLowering, AGotRelocationInADataItemIsRefused) {
    auto const t = target("x86_64");
    auto const f = format("elf64-x86_64-linux-exec");
    ASSERT_TRUE(t && f);
    AssembledModule in = moduleWith({});
    AssembledData d;
    d.symbol = SymbolId{9};
    d.section = DataSectionKind::Data;
    d.bytes.assign(8, 0);
    d.relocations.push_back(Relocation{0, SymbolId{2}, kindNamed(*t, "gotpcrel32"), 0});
    in.dataItems.push_back(d);
    AssembledModule out;
    DiagnosticReporter rep;
    (void)linker::lowerGotSlotReferences(in, out, *t, *f, rep);
    EXPECT_TRUE(rep.hasErrors()) << "an instruction-field relocation in a data item";
}

// ══ B. THE REAL MEMBERS, THROUGH THE DRIVER'S LINK ═══════════════════════════

TEST(GotSlotLowering, EveryX86GotTypeLinksIntoAnExecAndEachSlotHoldsItsSymbol) {
    auto const t = target("x86_64");
    auto const reader = format("elf64-x86_64-linux-staticlib");
    auto const image = format("elf64-x86_64-linux-exec");
    ASSERT_TRUE(t && reader && image);
    auto members = readMembers(*t, *reader, {dss::test::gccX8664DefaultMember(),
                                             dss::test::gccX8664CallsMember(),
                                             dss::test::gccX8664PlainMember()});
    // Premise: the three members carry the three wire types, read as three kinds.
    std::map<std::string, int> kinds;
    for (auto const& m : members.modules) {
        for (auto const& fn : m.functions) {
            for (auto const& rel : fn.relocations) {
                if (auto const* tri = t->relocationInfo(rel.kind);
                    tri != nullptr && tri->formulaKind == RelocFormulaKind::X86_64GotPcRel) {
                    ++kinds[tri->name];
                }
            }
        }
    }
    EXPECT_EQ(kinds["gotpcrel32"], 3) << "the -mrelax-relocations=no member: GOTPCREL";
    EXPECT_EQ(kinds["gotpcrelx32"], 1) << "the -fno-plt call: GOTPCRELX";
    EXPECT_EQ(kinds["rex_gotpcrelx32"], 2) << "the default member's function address "
                                              "and the -fPIC member's datum: REX_GOTPCRELX";
    auto const sites = members.sites;
    auto const linked = linkWithProgram(std::move(members), false, *t, *image);
    ASSERT_FALSE(linked.bytes.empty())
        << "every x86 GOT type must LINK through `linker::link`. Got:\n" << linked.errors;
    auto const secs = sectionsOf(linked.bytes);
    for (auto const& s : sites) {
        auto const slot = slotNamedBy(linked.bytes, s);
        ASSERT_TRUE(slot.has_value()) << s.function;
        auto const at = fileOffsetOf(secs, *slot);
        ASSERT_TRUE(at.has_value()) << s.function << " names no file-backed slot";
        auto const want = addressOf(linked.bytes, s.target);
        ASSERT_TRUE(want.has_value()) << s.target;
        EXPECT_EQ(rd(linked.bytes, *at, 8), *want)
            << s.function << "+" << s.offset << ": the slot must hold " << s.target
            << "'s address (a non-PIE exec fills it at link time)";
    }
}

TEST(GotSlotLowering, EveryAarch64GotPairLinksIntoAnExecAndBothHalvesNameOneSlot) {
    auto const t = target("arm64");
    auto const reader = format("elf64-aarch64-linux-staticlib");
    auto const image = format("elf64-aarch64-linux-exec");
    ASSERT_TRUE(t && reader && image);
    auto members = readMembers(*t, *reader, {dss::test::gccAarch64DefaultMember(),
                                             dss::test::gccAarch64CallsMember(),
                                             dss::test::gccAarch64PlainMember()});
    auto const sites = members.sites;
    ASSERT_EQ(sites.size(), 16u) << "eight GOT pairs across the three members";
    auto const pairs = pairUp(sites);
    ASSERT_EQ(pairs.size(), 8u) << "every ADRP half has its LDR half";
    auto const linked = linkWithProgram(std::move(members), true, *t, *image);
    ASSERT_FALSE(linked.bytes.empty())
        << "the aarch64 GOT pair must LINK through `linker::link`. Got:\n" << linked.errors;
    auto const secs = sectionsOf(linked.bytes);
    for (auto const& [i, ldrOffset] : pairs) {
        auto const slot = slotNamedBy(linked.bytes, sites[i], ldrOffset);
        ASSERT_TRUE(slot.has_value()) << sites[i].function;
        EXPECT_EQ(*slot % 8u, 0u) << "a 64-bit LDR's slot is 8-aligned";
        auto const at = fileOffsetOf(secs, *slot);
        ASSERT_TRUE(at.has_value())
            << sites[i].function << ": the ADRP page and the LDR page offset must name "
               "ONE file-backed slot";
        auto const want = addressOf(linked.bytes, sites[i].target);
        ASSERT_TRUE(want.has_value()) << sites[i].target;
        EXPECT_EQ(rd(linked.bytes, *at, 8), *want)
            << sites[i].function << ": the slot must hold " << sites[i].target;
    }
}

TEST(GotSlotLowering, APieFillsEachSlotThroughARelativeRow) {
    // AArch64 and x86_64 PIE: the slot of a definition in the image is
    // relocated by R_*_RELATIVE — base-relative, its addend the definition's
    // own (base-0) address, which the slot's prelinked bytes hold too.
    struct Case {
        char const* target; char const* reader; char const* image; bool arm64;
        std::vector<std::vector<std::uint8_t>> objects; std::uint32_t relative;
    };
    Case const cases[] = {
        {"arm64", "elf64-aarch64-linux-staticlib", "elf64-aarch64-linux-pie", true,
         {dss::test::gccAarch64DefaultMember()}, 1027u},  // R_AARCH64_RELATIVE
        {"x86_64", "elf64-x86_64-linux-staticlib", "elf64-x86_64-linux-pie", false,
         {dss::test::gccX8664PlainMember()}, 8u},          // R_X86_64_RELATIVE
    };
    for (auto const& c : cases) {
        auto const t = target(c.target);
        auto const reader = format(c.reader);
        auto const image = format(c.image);
        ASSERT_TRUE(t && reader && image);
        auto members = readMembers(*t, *reader, c.objects);
        auto const sites = members.sites;
        auto const pairs = pairUp(sites);
        auto const linked = linkWithProgram(std::move(members), c.arm64, *t, *image);
        ASSERT_FALSE(linked.bytes.empty()) << c.image << ":\n" << linked.errors;
        auto const rows = relaDyn(linked.bytes);
        std::size_t checked = 0;
        for (std::size_t i = 0; i < sites.size(); ++i) {
            if (c.arm64 && !pairs.contains(i)) continue;
            auto const slot = slotNamedBy(linked.bytes, sites[i],
                                          c.arm64 ? pairs.at(i) : 0);
            ASSERT_TRUE(slot.has_value());
            auto const want = addressOf(linked.bytes, sites[i].target);
            ASSERT_TRUE(want.has_value());
            bool covered = false;
            for (auto const& r : rows) {
                if (r.offset == *slot && r.type == c.relative
                    && static_cast<std::uint64_t>(r.addend) == *want) {
                    covered = true;
                }
            }
            EXPECT_TRUE(covered)
                << c.image << ": " << sites[i].function << "'s slot must be relocated "
                   "RELATIVE to " << sites[i].target << "'s address";
            ++checked;
        }
        EXPECT_EQ(checked, 3u) << c.image;
    }
}

TEST(GotSlotLowering, ASharedObjectFillsAPreemptibleDefinitionsSlotThroughTheSymbol) {
    // In a `.so` a DEFAULT-visibility definition can be interposed, so its
    // slot is resolved by the LOADER against the symbol (GNU ld: GLOB_DAT) —
    // never prelinked to this image's own copy.
    auto const t = target("arm64");
    auto const reader = format("elf64-aarch64-linux-staticlib");
    auto const image = format("elf64-aarch64-linux-dyn");
    ASSERT_TRUE(t && reader && image);
    auto members = readMembers(*t, *reader, {dss::test::gccAarch64PlainMember()});
    auto const sites = members.sites;
    auto const pairs = pairUp(sites);
    ASSERT_EQ(pairs.size(), 3u);
    AssembledModule lib = programModule(true);
    lib.userEntrySymbol.reset();   // a library has no entry
    std::vector<AssembledModule> mods;
    mods.push_back(std::move(lib));
    for (auto& m : members.modules) mods.push_back(std::move(m));
    DiagnosticReporter rep;
    auto const linked = linker::link(std::span<AssembledModule const>{mods.data(), mods.size()},
                                     *t, *image, rep);
    ASSERT_FALSE(rep.hasErrors()) << errorText(rep);
    auto const rows = relaDyn(linked.bytes);
    for (auto const& [i, ldrOffset] : pairs) {
        auto const slot = slotNamedBy(linked.bytes, sites[i], ldrOffset);
        ASSERT_TRUE(slot.has_value());
        bool bySymbol = false;
        for (auto const& r : rows) {
            if (r.offset == *slot && r.symbol == sites[i].target) bySymbol = true;
        }
        EXPECT_TRUE(bySymbol)
            << sites[i].function << "'s slot must be filled by a dynamic row against `"
            << sites[i].target << "`, the preemptible definition";
    }
}

// ══ C. A LIBRARY DATUM ═══════════════════════════════════════════════════════

TEST(GotSlotLowering, ALibraryDatumReachedThroughTheGotLinksAndTheLoaderFillsItsSlot) {
    // gcc -fPIC (x86_64) and gcc's default aarch64 code read `stdout` through
    // its GOT slot. The link mints the slot like any other, and a DATA import's
    // slot is filled by a row against the symbol — the loader writes the
    // datum's address, which is exactly what the member's load expects.
    struct Case {
        char const* target; char const* reader; char const* image; bool arm64;
        std::vector<std::uint8_t> object;
    };
    Case const cases[] = {
        {"x86_64", "elf64-x86_64-linux-staticlib", "elf64-x86_64-linux-exec", false,
         dss::test::gccX8664PicReadsStdoutMember()},
        {"arm64", "elf64-aarch64-linux-staticlib", "elf64-aarch64-linux-exec", true,
         dss::test::gccAarch64ReadsStdoutMember()},
    };
    for (auto const& c : cases) {
        auto const t = target(c.target);
        auto const reader = format(c.reader);
        auto const image = format(c.image);
        ASSERT_TRUE(t && reader && image);
        auto members = readMembers(*t, *reader, {c.object});
        bindStdoutToLibc(members);
        auto const sites = members.sites;
        ASSERT_EQ(sites.size(), c.arm64 ? 4u : 2u)
            << c.target << ": the member's premise — `stdout` through the GOT, twice";
        auto const pairs = pairUp(sites);
        DiagnosticReporter rep;
        auto const bytes = linkWithAppFn(std::move(members), c.arm64, *t, *image, rep);
        ASSERT_FALSE(bytes.empty())
            << c.target << ": a GOT reference to a library datum must LINK. Got:\n"
            << errorText(rep);
        auto const rows = relaDyn(bytes);
        std::optional<std::uint64_t> theSlot;
        for (std::size_t i = 0; i < sites.size(); ++i) {
            if (c.arm64 && !pairs.contains(i)) continue;
            auto const slot = slotNamedBy(bytes, sites[i], c.arm64 ? pairs.at(i) : 0);
            ASSERT_TRUE(slot.has_value()) << c.target << " " << sites[i].function;
            if (theSlot.has_value()) {
                EXPECT_EQ(*slot, *theSlot) << c.target << ": one slot per (symbol, addend)";
            }
            theSlot = *slot;
            bool bySymbol = false;
            for (auto const& r : rows) {
                if (r.offset == *slot && r.symbol == "stdout") bySymbol = true;
            }
            EXPECT_TRUE(bySymbol)
                << c.target << " " << sites[i].function << ": the slot the member loads "
                   "must be filled by the loader, through a row against `stdout`";
        }
        EXPECT_TRUE(theSlot.has_value()) << c.target;
    }
}

TEST(GotSlotLowering, ADirectReferenceToALibraryDatumIsRefusedByName) {
    // gcc's DEFAULT x86_64 code (PIE) reads `stdout` with `movq stdout(%rip)` —
    // an R_X86_64_PC32 that assumes the final link makes a COPY relocation (gcc
    // does, -no-pie and -pie alike). DSS makes none, and before this refusal
    // every image writer bound the reference to the datum's loader-filled slot:
    // the member read the slot instead of the datum (✔MEASURED 2026-09-24
    // through the CLI: exit 2, debug and release, exec and PIE).
    auto const t = target("x86_64");
    auto const reader = format("elf64-x86_64-linux-staticlib");
    auto const image = format("elf64-x86_64-linux-exec");
    ASSERT_TRUE(t && reader && image);
    auto members = readMembers(*t, *reader, {dss::test::gccX8664PieReadsStdoutMember()});
    ASSERT_TRUE(members.sites.empty()) << "the member's premise: no GOT reference at all";
    bindStdoutToLibc(members);
    DiagnosticReporter rep;
    auto const bytes = linkWithAppFn(std::move(members), false, *t, *image, rep);
    EXPECT_TRUE(bytes.empty()) << "no image may be emitted";
    EXPECT_TRUE(reported(rep, DiagnosticCode::K_ImportReferenceUnbindable)) << errorText(rep);
    auto const text = errorText(rep);
    EXPECT_NE(text.find("'stdout'"), std::string::npos) << text;
    EXPECT_NE(text.find("libc.so.6"), std::string::npos) << text;
    EXPECT_NE(text.find("copy relocation"), std::string::npos) << text;
}

TEST(GotSlotLowering, AMemberTheGotLoweringNowAdmitsIsStillRefusedForItsDirectDatum) {
    // The member this refusal exists for most: gcc's default x86_64 code that
    // reads `stdout` DIRECTLY and also takes the address of a program function
    // THROUGH THE GOT (R_X86_64_REX_GOTPCRELX). Before this change the GOT type
    // refused it at read; once the lowering admitted it, the direct datum read
    // would have linked into a silent wrong answer (✔MEASURED 2026-09-24: exit
    // 2). The GOT half is fine — the refusal is the datum's, by name.
    auto const t = target("x86_64");
    auto const reader = format("elf64-x86_64-linux-staticlib");
    auto const image = format("elf64-x86_64-linux-exec");
    ASSERT_TRUE(t && reader && image);
    auto members = readMembers(*t, *reader, {dss::test::gccX8664PieMixedMember()});
    ASSERT_EQ(members.sites.size(), 1u) << "the member's premise: one GOT reference, to app_fn";
    EXPECT_EQ(members.sites[0].target, "app_fn");
    bindStdoutToLibc(members);
    DiagnosticReporter rep;
    auto const bytes = linkWithAppFn(std::move(members), false, *t, *image, rep);
    EXPECT_TRUE(bytes.empty()) << "no image may be emitted";
    EXPECT_TRUE(reported(rep, DiagnosticCode::K_ImportReferenceUnbindable)) << errorText(rep);
    EXPECT_FALSE(reported(rep, DiagnosticCode::K_RelocationKindMismatch))
        << "the GOT reference itself is not what is refused:\n" << errorText(rep);
    EXPECT_NE(errorText(rep).find("'stdout'"), std::string::npos) << errorText(rep);
}

TEST(GotSlotLowering, AUnitThatReadsALibraryDatumThroughItsSlotIsNotJudged) {
    // The CONTROL: what MIR→LIR emits for `stdout` in an image is the same
    // relocation against the same import, read through the slot the writer
    // gives it — `ExternImport::readThroughSlot`, the one owner of that
    // statement. The same module with the statement withdrawn is the object
    // shape, and is refused.
    auto const t = target("x86_64");
    auto const image = format("elf64-x86_64-linux-exec");
    ASSERT_TRUE(t && image);
    auto const build = [&](bool throughSlot) {
        AssembledModule m;
        m.cuId = CompilationUnitId{1};
        AssembledFunction mainFn;
        mainFn.symbol = SymbolId{1};
        mainFn.bytes  = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x8B, 0x00, 0xC3};
        mainFn.relocations.push_back(Relocation{3, SymbolId{2}, kindNamed(*t, "rel32"), 0});
        m.functions = {mainFn};
        m.expectedFuncCount = 1;
        ExternImport ext;
        ext.symbol          = SymbolId{2};
        ext.mangledName     = "stdout";
        ext.libraryPath     = "libc.so.6";
        ext.isData          = true;
        ext.readThroughSlot = throughSlot;
        m.externImports.push_back(ext);
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global,
                                         SymbolVisibility::Default});
        m.userEntrySymbol = SymbolId{1};
        return m;
    };
    {
        std::vector<AssembledModule> mods{build(true)};
        DiagnosticReporter rep;
        (void)linker::link(std::span<AssembledModule const>{mods.data(), mods.size()},
                           *t, *image, rep);
        EXPECT_FALSE(reported(rep, DiagnosticCode::K_ImportReferenceUnbindable))
            << "a slot read is not judged:\n" << errorText(rep);
        EXPECT_FALSE(rep.hasErrors()) << errorText(rep);
    }
    {
        std::vector<AssembledModule> mods{build(false)};
        DiagnosticReporter rep;
        (void)linker::link(std::span<AssembledModule const>{mods.data(), mods.size()},
                           *t, *image, rep);
        EXPECT_TRUE(reported(rep, DiagnosticCode::K_ImportReferenceUnbindable))
            << "the same reference, direct, needs a copy relocation:\n" << errorText(rep);
    }
}
