// ★★★ WHAT A REFERENCE NAMING A WEAK SYMBOL RESOLVED TO NOTHING COMPUTES
// (P69, lane `lm`, D-LK-WEAK-UNDEFINED-SYMBOL-NAMED-DIRECTLY-IS-NOT-ADDRESS-ZERO).
//
// An image binds a WEAK symbol that no linked unit defines and no library binds
// to NOTHING, whose value is 0. A reference that reads the symbol THROUGH A SLOT
// (DSS's own data references, every GOT load) reads a slot holding 0; a
// reference that NAMES it directly computes its field from 0 where the image
// lets that field class reach it (`weakResolvedToNothing` in the image
// document) and is refused by name where it does not. Until P69 the null slot
// WAS the symbol, so a direct field computed the slot's own address: ✔MEASURED
// 2026-10-07, `extern int wd __attribute__((weak)); int *p = &wd;` linked on
// all five images and the pe64 program returned `p != NULL`.
//
// Every number here is read back off the EMITTED image: a field is found by the
// marker bytes planted beside it, and its value, its address and the load-time
// fix-ups naming it are read from the image's own headers. Each zero has a
// CONTROL the same instrument reads differently — a defined datum's pointer is
// non-zero and carries a fix-up — so a reader that saw nothing fails the
// control instead of passing the pin. The run witnesses are the examples
// `weak_undefined_address_in_static_data` and `weak_undefined_named_directly_exec`.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link/weak_resolved_to_nothing.hpp"
#include "program/program.hpp"

#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

struct Schemas {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

Schemas shipped(std::string_view arch, std::string_view format) {
    Schemas s;
    auto t = TargetSchema::loadShipped(arch);
    EXPECT_TRUE(t.has_value()) << "cannot load shipped target " << arch;
    if (t.has_value()) s.target = *t;
    auto f = ObjectFormatSchema::loadShipped(format);
    EXPECT_TRUE(f.has_value()) << "cannot load shipped format " << format;
    if (f.has_value()) s.format = *f;
    return s;
}

RelocationKind kindNamed(TargetSchema const& t, std::string_view name) {
    auto const* row = t.relocationByName(std::string{name});
    EXPECT_NE(row, nullptr) << name;
    return row != nullptr ? row->kind : RelocationKind{};
}

TargetRelocationInfo const& rowNamed(TargetSchema const& t, std::string_view name) {
    auto const* row = t.relocationByName(std::string{name});
    if (row == nullptr) throw std::runtime_error("no relocation row " + std::string{name});
    return *row;
}

constexpr std::uint32_t kWeak    = 99;   // the weak row's symbol
constexpr std::uint32_t kPointer = 2;    // the data item that holds a pointer
constexpr std::uint32_t kDefined = 7;    // a defined datum: the control

// Every image these tests link: a Mach-O image's code-signature identity is a
// function of the artifact's file name, which the link refuses to invent.
ImageRequest const kRequest{.artifactFileName = "weak_null_probe"};

ExternImport weakRow(std::string name, ExternKindOrigin origin = ExternKindOrigin::Pending,
                     bool isData = false) {
    ExternImport e;
    e.symbol      = SymbolId{kWeak};
    e.mangledName = std::move(name);
    e.kindOrigin  = origin;
    e.isData      = isData;
    e.binding     = SymbolBinding::Weak;
    return e;
}

std::vector<ParseDiagnostic> withCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    std::vector<ParseDiagnostic> out;
    for (auto const& d : rep.all()) {
        if (d.code == code) out.push_back(d);
    }
    return out;
}

bool mentions(ParseDiagnostic const& d, std::string_view text) {
    return d.actual.find(text) != std::string::npos;
}

std::string allErrors(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) out += "\n  " + d.actual;
    return out;
}

// ── the bytes the tests plant beside a field, to find it in the image ──────
// An instruction whose IMMEDIATE no writer produces: x86_64 `movabsq
// $0x0123456789abcdef, %rcx`; arm64 `movz x9, #0xbeef; movk x9, #0xcafe, lsl
// #16`. (A pair of trap instructions will not do: ✔MEASURED, `ud2; ud2` and
// `brk #0; brk #0` were not unique in the images these tests link — INFERRED to
// be a writer's trap padding.) `findOnce` refuses a marker that is not unique.
std::vector<std::uint8_t> const kX64Marker{0x48, 0xB9, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01};
std::vector<std::uint8_t> const kA64Marker{0xE9, 0xDD, 0x97, 0xD2, 0xC9, 0x5F, 0xB9, 0xF2};
// A data item's second word: the pointer under test is the 8 bytes before it.
std::vector<std::uint8_t> const kDataMarker{0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};

std::vector<std::uint8_t> cat(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (auto const& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// One unit whose function is `bytes`, carrying `rels`, and naming `rows`.
AssembledModule unit(std::uint32_t cu, std::string name, std::vector<std::uint8_t> bytes,
                     std::vector<Relocation> rels, std::vector<ExternImport> rows,
                     bool entry = true) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol      = SymbolId{1};
    fn.bytes       = std::move(bytes);
    fn.relocations = std::move(rels);
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, std::move(name), SymbolBinding::Global,
                                     SymbolVisibility::Default});
    if (entry) m.userEntrySymbol = SymbolId{1};
    for (auto& r : rows) m.externImports.push_back(std::move(r));
    return m;
}

// Adds a 16-byte data item: an 8-byte pointer to `target` through `kind` with
// `addend`, then `kDataMarker`.
void addPointer(AssembledModule& m, SymbolId target, RelocationKind kind, std::int64_t addend) {
    AssembledData d;
    d.symbol    = SymbolId{kPointer};
    d.section   = DataSectionKind::Data;
    d.bytes     = cat({std::vector<std::uint8_t>(8, 0), kDataMarker});
    d.alignment = Alignment::ofRuntimePow2(8);
    d.relocations.push_back(Relocation{0u, target, kind, addend});
    m.dataItems.push_back(std::move(d));
    m.symbols.push_back(ModuleSymbol{SymbolId{kPointer}, "pointer_under_test",
                                     SymbolBinding::Global, SymbolVisibility::Default});
}

// The CONTROL's defined datum.
void addDefinedDatum(AssembledModule& m) {
    AssembledData d;
    d.symbol    = SymbolId{kDefined};
    d.section   = DataSectionKind::Data;
    d.bytes     = {42, 0, 0, 0, 0, 0, 0, 0};
    d.alignment = Alignment::ofRuntimePow2(8);
    m.dataItems.push_back(std::move(d));
    m.symbols.push_back(ModuleSymbol{SymbolId{kDefined}, "defined_datum", SymbolBinding::Global,
                                     SymbolVisibility::Default});
}

// `main: <marker>; <insn>; return 0`.
std::vector<std::uint8_t> x64Main(std::vector<std::uint8_t> const& insn) {
    return cat({kX64Marker, insn, {0x31, 0xC0, 0xC3}});   // xor eax,eax; ret
}
std::vector<std::uint8_t> a64Main(std::vector<std::uint8_t> const& insn) {
    return cat({kA64Marker, insn, {0x00, 0x00, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6}});   // mov w0,#0; ret
}

// A unit whose `main` holds a pointer to the weak row `w` (or, for the
// control, to a defined datum).
AssembledModule pointerUnit(Schemas const& s, bool toTheWeakRow, std::int64_t addend) {
    std::vector<std::uint8_t> const body = s.target->name() == "arm64"
                                               ? std::vector<std::uint8_t>{0x00, 0x00, 0x80, 0x52,
                                                                           0xC0, 0x03, 0x5F, 0xD6}
                                               : std::vector<std::uint8_t>{0x31, 0xC0, 0xC3};
    std::vector<ExternImport> rows;
    if (toTheWeakRow) rows.push_back(weakRow("w"));
    auto m = unit(1, "main", body, {}, std::move(rows));
    if (!toTheWeakRow) addDefinedDatum(m);
    addPointer(m, SymbolId{toTheWeakRow ? kWeak : kDefined}, kindNamed(*s.target, "abs64"), addend);
    return m;
}

// ── reading the emitted image ───────────────────────────────────────────────
std::uint16_t u16(std::vector<std::uint8_t> const& b, std::uint64_t o) {
    return static_cast<std::uint16_t>(b.at(o) | (b.at(o + 1) << 8));
}
std::uint32_t u32(std::vector<std::uint8_t> const& b, std::uint64_t o) {
    std::uint32_t v = 0;
    for (std::uint64_t i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b.at(o + i)) << (8 * i);
    return v;
}
std::uint64_t u64(std::vector<std::uint8_t> const& b, std::uint64_t o) {
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b.at(o + i)) << (8 * i);
    return v;
}
std::string cstr(std::vector<std::uint8_t> const& b, std::uint64_t o, std::size_t max) {
    std::string s;
    for (std::size_t i = 0; i < max && o + i < b.size() && b[o + i] != 0; ++i) {
        s.push_back(static_cast<char>(b[o + i]));
    }
    return s;
}

// The one offset where `pattern` occurs; nothing when it occurs zero times or
// more than once.
std::optional<std::uint64_t> findOnce(std::vector<std::uint8_t> const& b,
                                      std::vector<std::uint8_t> const& pattern) {
    std::optional<std::uint64_t> at;
    auto it = b.begin();
    while (true) {
        it = std::search(it, b.end(), pattern.begin(), pattern.end());
        if (it == b.end()) break;
        if (at.has_value()) return std::nullopt;
        at = static_cast<std::uint64_t>(it - b.begin());
        ++it;
    }
    return at;
}

// A section: where its bytes are in the file and where they are in memory
// (an ELF section header, or a PE section header with `addr` its RVA).
struct Sec {
    std::string   name;
    std::uint64_t addr = 0, offset = 0, size = 0;
    std::uint32_t type = 0;   // ELF only
};

constexpr std::uint32_t kShtRela   = 4;
constexpr std::uint32_t kShtNobits = 8;

std::vector<Sec> elfSections(std::vector<std::uint8_t> const& b) {
    std::vector<Sec> out;
    if (b.size() < 64 || b[0] != 0x7F || b[1] != 'E') return out;
    std::uint64_t const shoff = u64(b, 40);
    std::uint16_t const shnum = u16(b, 60);
    std::uint16_t const shstr = u16(b, 62);
    if (shoff == 0 || shnum == 0) return out;
    std::uint64_t const strOff = u64(b, shoff + shstr * 64ull + 24);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const h = shoff + i * 64ull;
        Sec s;
        s.name   = cstr(b, strOff + u32(b, h), 64);
        s.type   = u32(b, h + 4);
        s.addr   = u64(b, h + 16);
        s.offset = u64(b, h + 24);
        s.size   = u64(b, h + 32);
        out.push_back(std::move(s));
    }
    return out;
}

std::optional<std::uint64_t> addrOfOffset(std::vector<Sec> const& secs, std::uint64_t off) {
    for (auto const& s : secs) {
        if (s.type == kShtNobits || s.addr == 0) continue;
        if (off >= s.offset && off < s.offset + s.size) return s.addr + (off - s.offset);
    }
    return std::nullopt;
}

std::optional<std::uint64_t> offsetOfAddr(std::vector<Sec> const& secs, std::uint64_t addr) {
    for (auto const& s : secs) {
        if (s.type == kShtNobits || s.addr == 0) continue;
        if (addr >= s.addr && addr < s.addr + s.size) return s.offset + (addr - s.addr);
    }
    return std::nullopt;
}

// Every `r_offset` of every RELA section: the image's load-time fix-ups.
std::vector<std::uint64_t> elfFixupAddresses(std::vector<std::uint8_t> const& b,
                                             std::vector<Sec> const& secs) {
    std::vector<std::uint64_t> out;
    for (auto const& s : secs) {
        if (s.type != kShtRela) continue;
        for (std::uint64_t e = 0; e + 24 <= s.size; e += 24) out.push_back(u64(b, s.offset + e));
    }
    return out;
}

struct Pe {
    std::uint32_t    relocRva = 0, relocSize = 0;
    std::vector<Sec> secs;
};

std::optional<Pe> readPe(std::vector<std::uint8_t> const& b) {
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return std::nullopt;
    std::uint32_t const pe = u32(b, 0x3C);
    if (u32(b, pe) != 0x00004550u) return std::nullopt;
    std::uint64_t const coff  = pe + 4ull;
    std::uint16_t const nsec  = u16(b, coff + 2);
    std::uint16_t const optSz = u16(b, coff + 16);
    std::uint64_t const opt   = coff + 20;
    if (u16(b, opt) != 0x20Bu) return std::nullopt;   // PE32+
    Pe out;
    out.relocRva  = u32(b, opt + 112 + 5 * 8);       // DataDirectory[BASERELOC]
    out.relocSize = u32(b, opt + 112 + 5 * 8 + 4);
    for (std::uint16_t i = 0; i < nsec; ++i) {
        std::uint64_t const h = opt + optSz + i * 40ull;
        Sec s;
        s.name   = cstr(b, h, 8);
        s.size   = std::min(u32(b, h + 8), u32(b, h + 16));   // the bytes the file holds
        s.addr   = u32(b, h + 12);
        s.offset = u32(b, h + 20);
        out.secs.push_back(std::move(s));
    }
    return out;
}

// The RVA of every base relocation the image asks the loader to apply.
std::vector<std::uint64_t> peBaseRelocationRvas(std::vector<std::uint8_t> const& b, Pe const& pe) {
    std::vector<std::uint64_t> out;
    if (pe.relocSize == 0) return out;
    auto const at = offsetOfAddr(pe.secs, pe.relocRva);
    if (!at.has_value()) return out;
    std::uint64_t p = *at;
    std::uint64_t const end = *at + pe.relocSize;
    while (p + 8 <= end) {
        std::uint32_t const page = u32(b, p);
        std::uint32_t const size = u32(b, p + 4);
        if (size < 8) break;
        for (std::uint64_t e = p + 8; e + 2 <= p + size; e += 2) {
            std::uint16_t const entry = u16(b, e);
            if ((entry >> 12) == 0) continue;   // IMAGE_REL_BASED_ABSOLUTE: padding
            out.push_back(page + (entry & 0x0FFFu));
        }
        p += size;
    }
    return out;
}

bool contains(std::vector<std::uint64_t> const& v, std::uint64_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// The 8-byte pointer before `kDataMarker`: its file offset and its value.
struct Pointer {
    std::uint64_t offset = 0;
    std::uint64_t value  = 0;
};
std::optional<Pointer> pointerInImage(std::vector<std::uint8_t> const& b) {
    auto const m = findOnce(b, kDataMarker);
    if (!m.has_value() || *m < 8) return std::nullopt;
    return Pointer{*m - 8, u64(b, *m - 8)};
}

// The address a 4-byte x86_64 displacement that ENDS its instruction reaches.
std::optional<std::uint64_t> x64Reaches(std::vector<std::uint8_t> const& b,
                                        std::vector<Sec> const& secs, std::uint64_t field) {
    auto const at = addrOfOffset(secs, field);
    if (!at.has_value()) return std::nullopt;
    auto const disp = static_cast<std::int32_t>(u32(b, field));
    return *at + 4 + static_cast<std::uint64_t>(static_cast<std::int64_t>(disp));
}

}  // namespace

// ═══ (1) the rule, read off the header ════════════════════════════════════

namespace {

std::string classOf(Schemas const& s, Relocation const& rel, std::vector<std::uint8_t> const& code,
                    bool inCode, bool readsThroughSlot = false) {
    linker::BranchSites const sites{*s.target};
    return linker::weakNullReferenceClassName(linker::classifyWeakNullReference(
        rel, std::span<std::uint8_t const>{code}, inCode, readsThroughSlot, *s.target, *s.format,
        sites));
}

}  // namespace

// Each class is a fact of HOW THE REFERENCE READS THE SYMBOL, read off the
// target's rows, the format's call rows and the target's branch encodings.
TEST(WeakResolvedToNothingRule, EveryReferenceIsClassifiedByHowItReadsTheSymbol) {
    auto const elf = shipped("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(elf.target && elf.format);
    auto const k = [&](char const* n) { return kindNamed(*elf.target, n); };
    std::vector<std::uint8_t> const lea{0x48, 0x8D, 0x05, 0, 0, 0, 0};
    std::vector<std::uint8_t> const call{0xE8, 0, 0, 0, 0};
    std::vector<std::uint8_t> const movImm{0xB8, 0, 0, 0, 0};
    SymbolId const w{kWeak};
    EXPECT_EQ(classOf(elf, Relocation{0, w, k("abs64"), 0}, {}, false), "an absolute field");
    EXPECT_EQ(classOf(elf, Relocation{1, w, k("abs32"), 0}, movImm, true), "an absolute field");
    EXPECT_EQ(classOf(elf, Relocation{3, w, k("riprel32"), 0}, lea, true),
              "a PC-relative field that is not a branch");
    EXPECT_EQ(classOf(elf, Relocation{0, w, k("pcrel32"), 0}, {}, false),
              "a PC-relative field that is not a branch");
    EXPECT_EQ(classOf(elf, Relocation{1, w, k("rel32"), 0}, call, true), "a branch")
        << "the format's call row says so";
    EXPECT_EQ(classOf(elf, Relocation{3, w, k("gotpcrel32"), -4}, lea, true),
              "a reference through a slot") << "a GOT load reads a slot whoever made it";
    EXPECT_EQ(classOf(elf, Relocation{3, w, k("riprel32"), 0}, lea, true, /*slot=*/true),
              "a reference through a slot") << "the unit's own statement";
    EXPECT_EQ(classOf(elf, Relocation{0, w, k("abs64"), 0}, {}, false, /*slot=*/true),
              "an absolute field") << "a data item's pointer names the symbol, whoever made it";
    EXPECT_EQ(classOf(elf, Relocation{3, w, k("tls-tpoff32"), 0}, lea, true),
              "a reference this rule does not judge");

    // PE spells a call and a displacement with ONE wire type; the instruction
    // decides, through the target's branch encodings.
    auto const pe = shipped("x86_64", "pe64-x86_64-windows-exec");
    ASSERT_TRUE(pe.target && pe.format);
    EXPECT_EQ(classOf(pe, Relocation{1, w, kindNamed(*pe.target, "rel32"), 0}, call, true), "a branch");
    EXPECT_EQ(classOf(pe, Relocation{3, w, kindNamed(*pe.target, "riprel32"), 0}, lea, true),
              "a PC-relative field that is not a branch");
    EXPECT_EQ(classOf(pe, Relocation{0, w, kindNamed(*pe.target, "imagerel32"), 0}, {}, false),
              "an image-relative (RVA) field");

    auto const a64 = shipped("arm64", "elf64-aarch64-linux-exec");
    ASSERT_TRUE(a64.target && a64.format);
    auto const ka = [&](char const* n) { return kindNamed(*a64.target, n); };
    std::vector<std::uint8_t> const word{0, 0, 0, 0x94};
    EXPECT_EQ(classOf(a64, Relocation{0, w, ka("call26"), 0}, word, true), "a branch");
    EXPECT_EQ(classOf(a64, Relocation{0, w, ka("adr_prel_pg_hi21"), 0}, word, true),
              "a PC-relative field that is not a branch");
    EXPECT_EQ(classOf(a64, Relocation{0, w, ka("add_abs_lo12_nc"), 0}, word, true),
              "a PC-relative field that is not a branch")
        << "a page offset belongs with the page reference it completes";
    EXPECT_EQ(classOf(a64, Relocation{0, w, ka("adr_got_page"), 0}, word, true),
              "a reference through a slot");
    EXPECT_EQ(classOf(a64, Relocation{0, w, ka("abs64"), 0}, {}, false), "an absolute field");
}

// The image documents: the ten images that refuse an undefined import answer
// as their reference linkers were measured to (the documents' comments carry
// the runs), and every other format answers nothing — the safe default.
TEST(WeakResolvedToNothingRule, EachImageAnswersAsItsReferenceLinkersWereMeasured) {
    using R = WeakNullReference;
    struct Want {
        char const* stem;
        R           absolute, pcRelative, branch;
    };
    constexpr Want kWant[] = {
        {"elf64-x86_64-linux-exec", R::Zero, R::Zero, R::Zero},
        {"elf64-x86_64-linux-pie", R::Zero, R::Refused, R::Refused},
        {"elf64-aarch64-linux-exec", R::Zero, R::Refused, R::NextInstruction},
        {"elf64-aarch64-linux-pie", R::Zero, R::Refused, R::NextInstruction},
        {"pe64-x86_64-windows-exec", R::Zero, R::Refused, R::Refused},
        {"pe64-x86_64-windows-dll", R::Zero, R::Refused, R::Refused},
        {"macho64-arm64-darwin-exec", R::Zero, R::Refused, R::Refused},
        {"macho64-arm64-darwin-dylib", R::Zero, R::Refused, R::Refused},
        {"macho64-x86_64-darwin-exec", R::Zero, R::Refused, R::Refused},
        {"macho64-x86_64-darwin-dylib", R::Zero, R::Refused, R::Refused},
    };
    for (auto const& w : kWant) {
        SCOPED_TRACE(w.stem);
        auto const f = ObjectFormatSchema::loadShipped(w.stem);
        ASSERT_TRUE(f.has_value());
        ASSERT_TRUE((*f)->isImageFlavor());
        EXPECT_FALSE((*f)->allowsUndefinedImports()) << "an image that binds a weak symbol to nothing";
        auto const a = (*f)->weakResolvedToNothing();
        EXPECT_EQ(a.absolute, w.absolute);
        EXPECT_EQ(a.pcRelative, w.pcRelative);
        EXPECT_EQ(a.branch, w.branch);
    }
    for (char const* stem : {"elf64-x86_64-linux", "elf64-x86_64-linux-dyn",
                             "elf64-aarch64-linux-dyn", "pe64-x86_64-windows-staticlib"}) {
        SCOPED_TRACE(stem);
        auto const f = ObjectFormatSchema::loadShipped(stem);
        ASSERT_TRUE(f.has_value());
        auto const a = (*f)->weakResolvedToNothing();
        EXPECT_EQ(a.absolute, R::Refused);
        EXPECT_EQ(a.pcRelative, R::Refused);
        EXPECT_EQ(a.branch, R::Refused);
    }
}

TEST(WeakResolvedToNothingRule, TheLinkWritesAnAbsoluteFieldFromZero) {
    auto const s = shipped("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    std::string why;
    std::vector<std::uint8_t> b(8, 0xEE);
    ASSERT_TRUE(linker::writeWeakNullConstant(
        b, Relocation{0, SymbolId{kWeak}, kindNamed(*s.target, "abs64"), 16},
        rowNamed(*s.target, "abs64"), linker::WeakNullReferenceClass::Absolute, why))
        << why;
    EXPECT_EQ(b, (std::vector<std::uint8_t>{16, 0, 0, 0, 0, 0, 0, 0})) << "0 + A";

    std::vector<std::uint8_t> c{0xB8, 0xEE, 0xEE, 0xEE, 0xEE};
    ASSERT_TRUE(linker::writeWeakNullConstant(
        c, Relocation{1, SymbolId{kWeak}, kindNamed(*s.target, "abs32"), -8},
        rowNamed(*s.target, "abs32"), linker::WeakNullReferenceClass::Absolute, why))
        << why;
    EXPECT_EQ(c, (std::vector<std::uint8_t>{0xB8, 0xF8, 0xFF, 0xFF, 0xFF}));

    // A 32-bit field cannot hold an addend of 2^31: refused, nothing written.
    std::vector<std::uint8_t> d{0xB8, 0xEE, 0xEE, 0xEE, 0xEE};
    EXPECT_FALSE(linker::writeWeakNullConstant(
        d, Relocation{1, SymbolId{kWeak}, kindNamed(*s.target, "abs32"), std::int64_t{1} << 31},
        rowNamed(*s.target, "abs32"), linker::WeakNullReferenceClass::Absolute, why));
    EXPECT_NE(why.find("cannot hold its own addend"), std::string::npos) << why;
    EXPECT_EQ(d, (std::vector<std::uint8_t>{0xB8, 0xEE, 0xEE, 0xEE, 0xEE}));
}

TEST(WeakResolvedToNothingRule, TheLinkWritesABranchToTheNextInstruction) {
    auto const s = shipped("arm64", "elf64-aarch64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    std::string why;
    std::vector<std::uint8_t> bl{0x00, 0x00, 0x00, 0x94};   // bl .
    ASSERT_TRUE(linker::writeWeakNullConstant(
        bl, Relocation{0, SymbolId{kWeak}, kindNamed(*s.target, "call26"), 0},
        rowNamed(*s.target, "call26"), linker::WeakNullReferenceClass::Branch, why))
        << why;
    EXPECT_EQ(u32(bl, 0), 0x94000001u) << "bl .+4: the call becomes a no-op (AAELF64)";

    // A field the assembler did not leave clear is not guessed at.
    std::vector<std::uint8_t> dirty{0x05, 0x00, 0x00, 0x94};
    EXPECT_FALSE(linker::writeWeakNullConstant(
        dirty, Relocation{0, SymbolId{kWeak}, kindNamed(*s.target, "call26"), 0},
        rowNamed(*s.target, "call26"), linker::WeakNullReferenceClass::Branch, why));
    EXPECT_NE(why.find("did not leave the branch's displacement field clear"), std::string::npos)
        << why;
    EXPECT_EQ(u32(dirty, 0), 0x94000005u) << "nothing written";
}

// ═══ (2) ELF x86_64 images ════════════════════════════════════════════════

// `int *p = &w;` (and `&w + 2`): the field holds 0 + A in an ET_EXEC and in a
// PIE, and the PIE asks the loader for NO fix-up of it — the CONTROL, a pointer
// to a defined datum, holds the datum's address and is fixed up at load in the
// PIE, so the instrument does see a fix-up where one exists.
TEST(WeakResolvedToNothingLink, AnAbsoluteDataPointerHoldsZeroPlusItsAddendWithNoFixup) {
    for (char const* stem : {"elf64-x86_64-linux-exec", "elf64-x86_64-linux-pie"}) {
        SCOPED_TRACE(stem);
        auto const s = shipped("x86_64", stem);
        ASSERT_TRUE(s.target && s.format);
        bool const pie = std::string_view{stem}.ends_with("pie");
        for (std::int64_t const addend : {std::int64_t{0}, std::int64_t{16}}) {
            DiagnosticReporter rep;
            auto const image = linker::link(pointerUnit(s, true, addend), *s.target, *s.format, rep, kRequest);
            ASSERT_TRUE(image.ok()) << allErrors(rep);
            auto const p = pointerInImage(image.bytes);
            ASSERT_TRUE(p.has_value());
            EXPECT_EQ(p->value, static_cast<std::uint64_t>(addend)) << "0 + A";
            auto const secs = elfSections(image.bytes);
            auto const at = addrOfOffset(secs, p->offset);
            ASSERT_TRUE(at.has_value());
            EXPECT_FALSE(contains(elfFixupAddresses(image.bytes, secs), *at))
                << "a constant the loader must not move";
        }
        DiagnosticReporter rep;
        auto const control = linker::link(pointerUnit(s, false, 0), *s.target, *s.format, rep, kRequest);
        ASSERT_TRUE(control.ok()) << allErrors(rep);
        auto const p = pointerInImage(control.bytes);
        ASSERT_TRUE(p.has_value());
        auto const secs = elfSections(control.bytes);
        auto const at = addrOfOffset(secs, p->offset);
        ASSERT_TRUE(at.has_value());
        if (pie) {
            EXPECT_TRUE(contains(elfFixupAddresses(control.bytes, secs), *at))
                << "CONTROL: a defined datum's address is fixed up in a PIE";
        } else {
            EXPECT_NE(p->value, 0u) << "CONTROL: a defined datum's address is not 0";
        }
    }
}

// `leaq w(%rip)` and `call w` in an ET_EXEC, which sits at its link address:
// each displacement reaches address 0.
TEST(WeakResolvedToNothingLink, APcRelativeFieldAndACallReachZeroInAnExec) {
    auto const s = shipped("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    struct Case {
        char const*               label;
        std::vector<std::uint8_t> insn;
        std::uint32_t             fieldAt;
        char const*               kind;
    };
    Case const kCases[] = {
        {"leaq w(%rip), %rax", {0x48, 0x8D, 0x05, 0, 0, 0, 0}, 3, "riprel32"},
        {"call w", {0xE8, 0, 0, 0, 0}, 1, "rel32"},
    };
    for (auto const& c : kCases) {
        SCOPED_TRACE(c.label);
        std::uint32_t const field = static_cast<std::uint32_t>(kX64Marker.size()) + c.fieldAt;
        auto const m = unit(1, "main", x64Main(c.insn),
                            {Relocation{field, SymbolId{kWeak}, kindNamed(*s.target, c.kind), 0}},
                            {weakRow("w")});
        DiagnosticReporter rep;
        auto const image = linker::link(m, *s.target, *s.format, rep, kRequest);
        ASSERT_TRUE(image.ok()) << allErrors(rep);
        auto const marker = findOnce(image.bytes, kX64Marker);
        ASSERT_TRUE(marker.has_value());
        // `field` counts from the function's first byte, which is the marker's.
        auto const reaches = x64Reaches(image.bytes, elfSections(image.bytes), *marker + field);
        ASSERT_TRUE(reaches.has_value());
        EXPECT_EQ(*reaches, 0u);
    }
}

// The ELF writer's OTHER ET_EXEC arm: an image that imports nothing is written
// by the static path, which gives a null-address symbol VA 0 on its own. No link
// reaches that arm with one today — every shipped ELF exec document exits
// through a libc import, so a linked image always imports `exit` — so the writer
// is driven directly, as the exec writer's own tests drive it (an untrampolined
// image, `imageEntryOverride = 0`). The CONTROL is the arm's refusal: a
// null-address symbol that is also a function's is refused by name, never
// given a second address.
TEST(WeakResolvedToNothingWriter, AnExecThatImportsNothingGivesANullAddressSymbolTheAddressZero) {
    auto const s = shipped("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    constexpr std::uint32_t kNullAddress = 98;
    std::uint32_t const field = static_cast<std::uint32_t>(kX64Marker.size()) + 3;
    auto m = unit(1, "main", x64Main({0x48, 0x8D, 0x05, 0, 0, 0, 0}),   // leaq w(%rip), %rax
                  {Relocation{field, SymbolId{kNullAddress}, kindNamed(*s.target, "riprel32"), 0}},
                  {});
    m.nullAddressSymbols.push_back(SymbolId{kNullAddress});
    m.imageEntryOverride = 0u;
    {
        DiagnosticReporter rep;
        auto const bytes = elf::encode(m, *s.target, *s.format, rep);
        ASSERT_FALSE(rep.hasErrors()) << allErrors(rep);
        auto const marker = findOnce(bytes, kX64Marker);
        ASSERT_TRUE(marker.has_value());
        auto const reaches = x64Reaches(bytes, elfSections(bytes), *marker + field);
        ASSERT_TRUE(reaches.has_value());
        EXPECT_EQ(*reaches, 0u) << "the displacement reaches address 0";
    }
    auto clash = m;
    clash.functions.front().relocations.clear();
    clash.nullAddressSymbols = {SymbolId{1}};   // `main`'s own symbol
    DiagnosticReporter rep;
    auto const bytes = elf::encode(clash, *s.target, *s.format, rep);
    EXPECT_TRUE(bytes.empty()) << "no image";
    auto const refused = withCode(rep, DiagnosticCode::K_SymbolUndefined);
    ASSERT_EQ(refused.size(), 1u) << allErrors(rep);
    EXPECT_TRUE(mentions(refused[0], "null-address symbol #1 collides")) << refused[0].actual;
}

// The same two in a PIE, which the loader moves: no displacement reaches 0, so
// each is refused by name — and `movl $w` (an absolute field the references
// keep dynamic and therefore refuse here) is a constant DSS writes.
TEST(WeakResolvedToNothingLink, InAPieADisplacementIsRefusedByNameAndAnAbsoluteFieldIsZero) {
    auto const s = shipped("x86_64", "elf64-x86_64-linux-pie");
    ASSERT_TRUE(s.target && s.format);
    struct Case {
        char const*               label;
        std::vector<std::uint8_t> insn;
        std::uint32_t             fieldAt;
        char const*               kind;
        char const*               cls;
    };
    Case const kCases[] = {
        {"leaq w(%rip), %rax", {0x48, 0x8D, 0x05, 0, 0, 0, 0}, 3, "riprel32", "a PC-relative field"},
        {"call w", {0xE8, 0, 0, 0, 0}, 1, "rel32", "a branch"},
    };
    for (auto const& c : kCases) {
        SCOPED_TRACE(c.label);
        std::uint32_t const field = static_cast<std::uint32_t>(kX64Marker.size()) + c.fieldAt;
        auto const m = unit(1, "main", x64Main(c.insn),
                            {Relocation{field, SymbolId{kWeak}, kindNamed(*s.target, c.kind), 0}},
                            {weakRow("w")});
        DiagnosticReporter rep;
        auto const image = linker::link(m, *s.target, *s.format, rep, kRequest);
        auto const refused = withCode(rep, DiagnosticCode::K_SymbolUndefined);
        ASSERT_EQ(refused.size(), 1u) << allErrors(rep);
        EXPECT_TRUE(mentions(refused[0], "'w'")) << refused[0].actual;
        EXPECT_TRUE(mentions(refused[0], c.cls)) << refused[0].actual;
        EXPECT_TRUE(mentions(refused[0], "answers `refused`")) << refused[0].actual;
        EXPECT_FALSE(image.ok());
    }
    std::uint32_t const imm = static_cast<std::uint32_t>(kX64Marker.size()) + 1;
    auto const m = unit(1, "main", x64Main({0xB8, 0xEE, 0xEE, 0xEE, 0xEE}),
                        {Relocation{imm, SymbolId{kWeak}, kindNamed(*s.target, "abs32"), 0}},
                        {weakRow("w")});
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep, kRequest);
    ASSERT_TRUE(image.ok()) << allErrors(rep);
    auto const marker = findOnce(image.bytes, kX64Marker);
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(u32(image.bytes, *marker + kX64Marker.size() + 1), 0u) << "movl $w -> movl $0";
}

// ═══ (3) two units, one name, read two ways ═══════════════════════════════
//
// The merge keeps ONE row per name, carrying the `readThroughSlot` of whichever
// unit landed first, so the gate reads how each INPUT unit reaches the name.

namespace {

// A compiled unit's slot read, `movq w(%rip), %rax`, naming its row, which says
// it reads through the slot.
AssembledModule slotReader(Schemas const& s, std::uint32_t cu, std::string name, bool entry) {
    auto row = weakRow("w", ExternKindOrigin::Stated, /*isData=*/true);
    row.readThroughSlot = true;
    return unit(cu, std::move(name), x64Main({0x48, 0x8B, 0x05, 0, 0, 0, 0}),
                {Relocation{static_cast<std::uint32_t>(kX64Marker.size() + 3), SymbolId{kWeak},
                            kindNamed(*s.target, "riprel32"), 0}},
                {std::move(row)}, entry);
}

// A unit that names `w` from code through `kind`.
AssembledModule otherUnit(Schemas const& s, std::uint32_t cu, std::string name, bool entry,
                          char const* kind, std::int64_t addend) {
    return unit(cu, std::move(name), {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3},
                {Relocation{3u, SymbolId{kWeak}, kindNamed(*s.target, kind), addend}},
                {weakRow("w")}, entry);
}

std::vector<AssembledModule> inOrder(bool slotReaderFirst, AssembledModule a, AssembledModule b) {
    std::vector<AssembledModule> v;
    if (slotReaderFirst) {
        v.push_back(std::move(a));
        v.push_back(std::move(b));
    } else {
        v.push_back(std::move(b));
        v.push_back(std::move(a));
    }
    return v;
}

}  // namespace

TEST(WeakResolvedToNothingRule, TheInputUnitsSayWhichNamesAreReadThroughASlotAndWhichTwoWays) {
    auto const s = shipped("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    std::vector<AssembledModule> two;
    two.push_back(slotReader(s, 1, "main", true));
    two.push_back(otherUnit(s, 2, "other", false, "riprel32", 0));
    auto const r = linker::weakSlotReadersOf(two, *s.target);
    EXPECT_TRUE(r.throughSlot.contains("w"));
    EXPECT_TRUE(r.twoWays.contains("w")) << "one reads the slot, the other names it directly";

    // A GOT load is not a direct reference.
    std::vector<AssembledModule> got;
    got.push_back(slotReader(s, 1, "main", true));
    got.push_back(otherUnit(s, 2, "other", false, "gotpcrel32", -4));
    auto const g = linker::weakSlotReadersOf(got, *s.target);
    EXPECT_TRUE(g.throughSlot.contains("w"));
    EXPECT_FALSE(g.twoWays.contains("w"));

    // One unit alone is never two ways.
    std::vector<AssembledModule> one;
    one.push_back(otherUnit(s, 2, "main", true, "riprel32", 0));
    auto const o = linker::weakSlotReadersOf(one, *s.target);
    EXPECT_FALSE(o.throughSlot.contains("w"));
    EXPECT_FALSE(o.twoWays.contains("w"));
}

TEST(WeakResolvedToNothingLink, ANameOneUnitReadsThroughItsSlotAndAnotherNamesDirectlyIsRefused) {
    auto const s = shipped("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    // CONTROLS: each unit alone links.
    {
        DiagnosticReporter rep;
        auto const image = linker::link(slotReader(s, 1, "main", true), *s.target, *s.format, rep, kRequest);
        EXPECT_TRUE(image.ok()) << "a slot reader alone" << allErrors(rep);
    }
    {
        DiagnosticReporter rep;
        auto const image =
            linker::link(otherUnit(s, 2, "main", true, "riprel32", 0), *s.target, *s.format, rep, kRequest);
        EXPECT_TRUE(image.ok()) << "a direct reference alone" << allErrors(rep);
    }
    for (bool const slotReaderFirst : {true, false}) {
        SCOPED_TRACE(slotReaderFirst ? "the slot reader first" : "the direct reference first");
        auto const mods = inOrder(slotReaderFirst, slotReader(s, 1, "main", true),
                                  otherUnit(s, 2, "other", false, "riprel32", 0));
        DiagnosticReporter rep;
        auto const image = linker::link(std::span<AssembledModule const>{mods.data(), mods.size()},
                                        *s.target, *s.format, rep, kRequest);
        auto const refused = withCode(rep, DiagnosticCode::K_SymbolUndefined);
        ASSERT_EQ(refused.size(), 1u) << allErrors(rep);
        EXPECT_TRUE(mentions(refused[0], "'w'")) << refused[0].actual;
        EXPECT_TRUE(mentions(refused[0], "THROUGH A SLOT")) << refused[0].actual;
        EXPECT_FALSE(image.ok());
    }
}

// The order the input-unit reading closes: with the GOT unit first, the merged
// row said "not read through a slot", and the compiled unit's slot read would
// be taken for a direct reference — retargeted to address 0 and dereferenced.
// In either order the slot read must reach a slot (not address 0) that holds 0.
TEST(WeakResolvedToNothingLink, ASlotReaderBesideAGotLoadReadsTheNullSlotInEitherOrder) {
    auto const s = shipped("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    for (bool const slotReaderFirst : {true, false}) {
        SCOPED_TRACE(slotReaderFirst ? "the slot reader first" : "the GOT load first");
        auto const mods = inOrder(slotReaderFirst, slotReader(s, 1, "main", true),
                                  otherUnit(s, 2, "other", false, "gotpcrel32", -4));
        DiagnosticReporter rep;
        auto const image = linker::link(std::span<AssembledModule const>{mods.data(), mods.size()},
                                        *s.target, *s.format, rep, kRequest);
        ASSERT_TRUE(image.ok()) << allErrors(rep);
        auto const secs = elfSections(image.bytes);
        auto const marker = findOnce(image.bytes, kX64Marker);
        ASSERT_TRUE(marker.has_value());
        auto const slot = x64Reaches(image.bytes, secs, *marker + kX64Marker.size() + 3);
        ASSERT_TRUE(slot.has_value());
        EXPECT_NE(*slot, 0u) << "a slot read reaches a SLOT, never address 0";
        auto const at = offsetOfAddr(secs, *slot);
        ASSERT_TRUE(at.has_value()) << "the slot is in the image";
        EXPECT_EQ(u64(image.bytes, *at), 0u) << "and the slot holds 0";
    }
}

// ═══ (4) ELF aarch64 images ═══════════════════════════════════════════════

// AAELF64: a CALL26 to an unresolved weak reference, without dynamic
// pre-emption, is a branch to the next instruction — in an exec and in a PIE,
// the branch's displacement being its own width.
TEST(WeakResolvedToNothingLink, AnArm64CallBecomesABranchToTheNextInstruction) {
    for (char const* stem : {"elf64-aarch64-linux-exec", "elf64-aarch64-linux-pie"}) {
        SCOPED_TRACE(stem);
        auto const s = shipped("arm64", stem);
        ASSERT_TRUE(s.target && s.format);
        std::uint32_t const at = static_cast<std::uint32_t>(kA64Marker.size());
        auto const m = unit(1, "main", a64Main({0x00, 0x00, 0x00, 0x94}),
                            {Relocation{at, SymbolId{kWeak}, kindNamed(*s.target, "call26"), 0}},
                            {weakRow("w")});
        DiagnosticReporter rep;
        auto const image = linker::link(m, *s.target, *s.format, rep, kRequest);
        ASSERT_TRUE(image.ok()) << allErrors(rep);
        auto const marker = findOnce(image.bytes, kA64Marker);
        ASSERT_TRUE(marker.has_value());
        EXPECT_EQ(u32(image.bytes, *marker + at), 0x94000001u) << "bl .+4";
    }
}

// The page pair is PC-relative: AAELF64 gives it the address of the PLACE,
// which no null test reads as absent, and 0 would contradict the ABI — refused.
// An absolute pointer holds 0.
TEST(WeakResolvedToNothingLink, AnArm64PagePairIsRefusedByNameAndAPointerIsZero) {
    auto const s = shipped("arm64", "elf64-aarch64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    std::uint32_t const at = static_cast<std::uint32_t>(kA64Marker.size());
    auto const m = unit(1, "main",
                        a64Main({0x00, 0x00, 0x00, 0x90,     // adrp x0, w
                                 0x00, 0x00, 0x00, 0x91}),   // add x0, x0, :lo12:w
                        {Relocation{at, SymbolId{kWeak}, kindNamed(*s.target, "adr_prel_pg_hi21"), 0},
                         Relocation{at + 4, SymbolId{kWeak}, kindNamed(*s.target, "add_abs_lo12_nc"), 0}},
                        {weakRow("w")});
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep, kRequest);
    auto const refused = withCode(rep, DiagnosticCode::K_SymbolUndefined);
    ASSERT_EQ(refused.size(), 1u) << "one report for the class, however many fields" << allErrors(rep);
    EXPECT_TRUE(mentions(refused[0], "'w'")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "a PC-relative field")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "AAELF64")) << refused[0].actual;
    EXPECT_FALSE(image.ok());

    DiagnosticReporter prep;
    auto const pointer = linker::link(pointerUnit(s, true, 0), *s.target, *s.format, prep, kRequest);
    ASSERT_TRUE(pointer.ok()) << allErrors(prep);
    auto const p = pointerInImage(pointer.bytes);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->value, 0u);
    DiagnosticReporter crep;
    auto const control = linker::link(pointerUnit(s, false, 0), *s.target, *s.format, crep, kRequest);
    ASSERT_TRUE(control.ok()) << allErrors(crep);
    auto const c = pointerInImage(control.bytes);
    ASSERT_TRUE(c.has_value());
    EXPECT_NE(c->value, 0u) << "CONTROL: a defined datum's address is not 0";
}

// ═══ (5) PE and Mach-O images ═════════════════════════════════════════════

// `int *p = &w;` on pe64: the field holds 0 and the image asks for NO base
// relocation of it (PE/COFF: an absolute symbol "is not an address"); the
// CONTROL's field is base-relocated. This is the shape that returned
// `p != NULL` before P69.
TEST(WeakResolvedToNothingLink, OnPeAnAbsolutePointerHoldsZeroWithNoBaseRelocation) {
    {
        auto const s = shipped("x86_64", "pe64-x86_64-windows-exec");
        ASSERT_TRUE(s.target && s.format);
        DiagnosticReporter rep;
        auto const image = linker::link(pointerUnit(s, true, 0), *s.target, *s.format, rep, kRequest);
        ASSERT_TRUE(image.ok()) << allErrors(rep);
        auto const pe = readPe(image.bytes);
        ASSERT_TRUE(pe.has_value());
        auto const p = pointerInImage(image.bytes);
        ASSERT_TRUE(p.has_value());
        EXPECT_EQ(p->value, 0u);
        auto const rva = addrOfOffset(pe->secs, p->offset);
        ASSERT_TRUE(rva.has_value());
        EXPECT_FALSE(contains(peBaseRelocationRvas(image.bytes, *pe), *rva));

        DiagnosticReporter crep;
        auto const control = linker::link(pointerUnit(s, false, 0), *s.target, *s.format, crep, kRequest);
        ASSERT_TRUE(control.ok()) << allErrors(crep);
        auto const cpe = readPe(control.bytes);
        ASSERT_TRUE(cpe.has_value());
        auto const c = pointerInImage(control.bytes);
        ASSERT_TRUE(c.has_value());
        EXPECT_NE(c->value, 0u);
        auto const crva = addrOfOffset(cpe->secs, c->offset);
        ASSERT_TRUE(crva.has_value());
        EXPECT_TRUE(contains(peBaseRelocationRvas(control.bytes, *cpe), *crva))
            << "CONTROL: a defined datum's address is base-relocated";
    }
}

// The image is based far from 0 and moves: neither a displacement nor a call
// reaches 0, and link.exe refuses both (LNK2016) — refused by name.
TEST(WeakResolvedToNothingLink, OnPeADisplacementAndACallAreRefusedByName) {
    auto const s = shipped("x86_64", "pe64-x86_64-windows-exec");
    ASSERT_TRUE(s.target && s.format);
    struct Case {
        char const*               label;
        std::vector<std::uint8_t> insn;
        std::uint32_t             fieldAt;
        char const*               kind;
        char const*               cls;
    };
    Case const kCases[] = {
        {"leaq w(%rip), %rax", {0x48, 0x8D, 0x05, 0, 0, 0, 0}, 3, "riprel32", "a PC-relative field"},
        {"call w", {0xE8, 0, 0, 0, 0}, 1, "rel32", "a branch"},
    };
    for (auto const& c : kCases) {
        SCOPED_TRACE(c.label);
        std::uint32_t const field = static_cast<std::uint32_t>(kX64Marker.size()) + c.fieldAt;
        auto const m = unit(1, "main", x64Main(c.insn),
                            {Relocation{field, SymbolId{kWeak}, kindNamed(*s.target, c.kind), 0}},
                            {weakRow("w")});
        DiagnosticReporter rep;
        auto const image = linker::link(m, *s.target, *s.format, rep, kRequest);
        auto const refused = withCode(rep, DiagnosticCode::K_SymbolUndefined);
        ASSERT_EQ(refused.size(), 1u) << allErrors(rep);
        EXPECT_TRUE(mentions(refused[0], "'w'")) << refused[0].actual;
        EXPECT_TRUE(mentions(refused[0], c.cls)) << refused[0].actual;
        EXPECT_FALSE(image.ok());
    }
}

// Mach-O: a missing weak import's address is NULL (Apple's documentation, and
// ✔MEASURED ld64 -U binding `.quad w` to 0), so the pointer holds 0 with no
// rebase — and a branch, which no displacement from an image above __PAGEZERO
// reaches 0 with, is refused (ld64 -U routes one through a stub, which DSS does
// not make for a symbol bound to nothing: the documents' comments carry the runs).
TEST(WeakResolvedToNothingLink, OnMachOAPointerHoldsZeroAndABranchIsRefused) {
    for (auto const& [arch, stem] : {std::pair<char const*, char const*>{"x86_64", "macho64-x86_64-darwin-exec"},
                                     std::pair<char const*, char const*>{"arm64", "macho64-arm64-darwin-exec"}}) {
        SCOPED_TRACE(stem);
        auto const s = shipped(arch, stem);
        ASSERT_TRUE(s.target && s.format);
        DiagnosticReporter rep;
        auto const image = linker::link(pointerUnit(s, true, 0), *s.target, *s.format, rep, kRequest);
        ASSERT_TRUE(image.ok()) << allErrors(rep);
        auto const p = pointerInImage(image.bytes);
        ASSERT_TRUE(p.has_value());
        EXPECT_EQ(p->value, 0u);
        DiagnosticReporter crep;
        auto const control = linker::link(pointerUnit(s, false, 0), *s.target, *s.format, crep, kRequest);
        ASSERT_TRUE(control.ok()) << allErrors(crep);
        auto const c = pointerInImage(control.bytes);
        ASSERT_TRUE(c.has_value());
        EXPECT_NE(c->value, 0u) << "CONTROL: a defined datum's pointer is a rebase, not 0";
    }
    auto const s = shipped("arm64", "macho64-arm64-darwin-exec");
    ASSERT_TRUE(s.target && s.format);
    std::uint32_t const at = static_cast<std::uint32_t>(kA64Marker.size());
    auto const m = unit(1, "main", a64Main({0x00, 0x00, 0x00, 0x94}),
                        {Relocation{at, SymbolId{kWeak}, kindNamed(*s.target, "call26"), 0}},
                        {weakRow("w")});
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep, kRequest);
    auto const refused = withCode(rep, DiagnosticCode::K_SymbolUndefined);
    ASSERT_EQ(refused.size(), 1u) << allErrors(rep);
    EXPECT_TRUE(mentions(refused[0], "a branch")) << refused[0].actual;
    EXPECT_FALSE(image.ok());
}

// ═══ (6) the native witness: a reference compiler's objects, linked by DSS ═══
//
// The row's own pins: a gcc object that names a weak datum and a weak function
// nothing defines DIRECTLY — `-fno-pie` x86_64 code is `movl $wd, %eax`
// (R_X86_64_32) and its statics `.quad wd` (R_X86_64_64), a guarded `call wf`
// (R_X86_64_PLT32) — and the same C through the GOT (`-fPIE`; aarch64 gcc
// reaches every weak symbol through the GOT even at -fno-pie), archived by the
// host's `ar` and linked under a DSS `main`, which reads every answer back.
// ✔MEASURED 2026-10-07 (probe run 20261007-070411-5f8a91bf) that GNU ld and
// ld.lld link the -fno-pie object -no-pie and give every answer 0, and REFUSE it
// -pie (R_X86_64_32 "can not be used when making a PIE object"): DSS links it
// into a PIE too, because the value is a constant that needs no fix-up — the
// PIE arm has no call, which DSS refuses from a FOREIGN object in a PIE (no
// displacement reaches 0; the references link it through a PLT entry whose slot
// the loader leaves 0 — D-LK-FOREIGN-CALL-TO-A-WEAK-SYMBOL-RESOLVED-TO-NOTHING-HAS-NO-STUB;
// DSS's own C calls a weak import through its slot since P69 round 4).
//   * Linux: the host's ISA, exec and PIE — x86_64 on the WSL leg, aarch64 on
//     the VPS leg.
//   * Windows and macOS: skipped, naming why (no ELF reference compiler; a
//     clang Mach-O object reaches a weak symbol's address through the GOT and
//     calls it through a stub, ✔MEASURED with ld64 in probe run
//     20261007-130113-4f4f1f1c — the GOT half pinned above by the image tests,
//     the call the same residual).

namespace {

namespace fs = std::filesystem;

constexpr char const* kAddressMember =
    "extern int wd __attribute__((weak));\n"
    "extern int wf(void) __attribute__((weak));\n"
    "int member_datum_is_null(void) { return &wd == 0; }\n"
    "int member_function_is_null(void) { return wf == 0; }\n"
    "int *member_datum_pointer = &wd;\n"
    "int (*member_function_pointer)(void) = wf;\n";
constexpr char const* kCallMember =
    "extern int wf(void) __attribute__((weak));\n"
    "int member_call_if_present(void) { return wf ? wf() : 7; }\n";

constexpr char const* kNativeMain =
    "int member_datum_is_null(void);\n"
    "int member_function_is_null(void);\n"
    "extern int *member_datum_pointer;\n"
    "extern int (*member_function_pointer)(void);\n"
    "int main(void) {\n"
    "    int bad = 0;\n"
    "    if (!member_datum_is_null()) bad |= 1;\n"
    "    if (!member_function_is_null()) bad |= 2;\n"
    "    if (member_datum_pointer != 0) bad |= 4;\n"
    "    if (member_function_pointer != 0) bad |= 8;\n"
    "    return bad == 0 ? 42 : 100 + bad;\n"
    "}\n";
constexpr char const* kNativeMainWithCall =
    "int member_datum_is_null(void);\n"
    "int member_function_is_null(void);\n"
    "int member_call_if_present(void);\n"
    "extern int *member_datum_pointer;\n"
    "extern int (*member_function_pointer)(void);\n"
    "int main(void) {\n"
    "    int bad = 0;\n"
    "    if (!member_datum_is_null()) bad |= 1;\n"
    "    if (!member_function_is_null()) bad |= 2;\n"
    "    if (member_datum_pointer != 0) bad |= 4;\n"
    "    if (member_function_pointer != 0) bad |= 8;\n"
    "    if (member_call_if_present() != 7) bad |= 16;\n"
    "    return bad == 0 ? 42 : 100 + bad;\n"
    "}\n";
constexpr char const* kNativeBadBits =
    "exit 100 + bits: 1 the member's `&wd == 0` was false, 2 its `wf == 0` was false, 4 its `int *p = &wd` was "
    "not null, 8 its `int (*p)(void) = wf` was not null, 16 its guarded call did not take the absent arm";

struct NativeImage {
    char const* spec;
    bool        withCall;
};
struct NativeArm {
    std::string             label;
    std::string             flags;
    std::vector<NativeImage> images;
};

[[nodiscard]] std::vector<NativeArm> nativeArms() {
#if defined(__linux__) && defined(__x86_64__)
    return {{"gcc -O0 -fno-pie (direct references)", "-O0 -fno-pie",
             {{"x86_64:elf64-x86_64-linux-exec", true}, {"x86_64:elf64-x86_64-linux-pie", false}}},
            {"gcc -O2 -fPIE (through the GOT)", "-O2 -fPIE",
             {{"x86_64:elf64-x86_64-linux-exec", true}, {"x86_64:elf64-x86_64-linux-pie", false}}}};
#elif defined(__linux__) && defined(__aarch64__)
    return {{"gcc -O0 -fno-pie (through the GOT, a CALL26)", "-O0 -fno-pie",
             {{"arm64:elf64-aarch64-linux-exec", true}, {"arm64:elf64-aarch64-linux-pie", true}}}};
#else
    return {};
#endif
}

void writeText(fs::path const& p, std::string_view text) {
    std::ofstream(p, std::ios::binary) << text;
}

[[nodiscard]] int runCapturing(std::string const& cmd, fs::path const& log) {
    return std::system(test_support::native_probe::captureCmd(cmd, log).c_str());
}

}  // namespace

TEST(WeakResolvedToNothingNative, AReferenceCompilersWeakReferencesReadZeroUnderADssLink) {
    auto const arms = nativeArms();
    if (arms.empty()) {
        GTEST_SKIP() << "no ELF reference compiler for this host's ISA (Windows and macOS: see the comment above)";
    }
    test_support::ScratchDir scratch{test_support::Location::Temp, "weak-resolved-to-nothing"};
    auto const dir = scratch.path();
    writeText(dir / "check.c", "int main(void){return 0;}\n");
    if (runCapturing("cc -o \"" + (dir / "check").string() + "\" \"" + (dir / "check.c").string() + "\"",
                     dir / "check.txt")
        != 0) {
        GTEST_SKIP() << "the host's `cc` cannot build a trivial program: "
                     << test_support::native_probe::tailOf(dir / "check.txt", 5);
    }
    writeText(dir / "address.c", kAddressMember);
    writeText(dir / "call.c", kCallMember);
    writeText(dir / "main.c", kNativeMain);
    writeText(dir / "main_call.c", kNativeMainWithCall);
    for (std::size_t k = 0; k < arms.size(); ++k) {
        auto const& arm = arms[k];
        SCOPED_TRACE(arm.label);
        fs::path const armDir = dir / ("arm" + std::to_string(k));
        fs::create_directories(armDir);
        for (char const* member : {"address", "call"}) {
            fs::path const obj = armDir / (std::string{member} + ".o");
            std::string const cc = "cc " + arm.flags + " -c -o \"" + obj.string() + "\" \""
                                 + (dir / (std::string{member} + ".c")).string() + "\"";
            ASSERT_EQ(runCapturing(cc, armDir / (std::string{member} + ".txt")), 0)
                << cc << "\n" << test_support::native_probe::tailOf(armDir / (std::string{member} + ".txt"), 10);
        }
        for (bool const withCall : {false, true}) {
            fs::path const archive = armDir / (withCall ? "libweakcall.a" : "libweak.a");
            std::string const ar = "ar rcs \"" + archive.string() + "\" \"" + (armDir / "address.o").string()
                                 + "\"" + (withCall ? " \"" + (armDir / "call.o").string() + "\"" : std::string{});
            ASSERT_EQ(runCapturing(ar, armDir / "ar.txt"), 0)
                << ar << "\n" << test_support::native_probe::tailOf(armDir / "ar.txt", 10);
        }
        for (auto const& image : arm.images) {
            SCOPED_TRACE(image.spec);
            fs::path const out = armDir / ("out-" + std::to_string(&image - arm.images.data()));
            fs::create_directories(out);
            fs::path const source = dir / (image.withCall ? "main_call.c" : "main.c");
            Program p;
            p.setOutputDir(out);
            p.setResolveLibraries(
                std::vector<fs::path>{armDir / (image.withCall ? "libweakcall.a" : "libweak.a")});
            DiagnosticReporter rep;
            int const rc = p.compileFiles(std::vector<std::string>{source.string()}, "c",
                                          std::vector<std::string>{image.spec}, rep);
            ASSERT_EQ(rc, 0) << allErrors(rep);
            // An ELF executable is named after its source's stem (✔MEASURED: `main_call.c`
            // linked to `out-0/main_call`).
            fs::path const exe = out / source.stem();
            ASSERT_TRUE(fs::exists(exe)) << allErrors(rep);
            auto const r = test_support::runBinary(exe);
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_FALSE(r.timedOut);
            EXPECT_EQ(r.exitCode, 42u) << kNativeBadBits;
            std::cout << "[native-arm] " << arm.label << " " << image.spec << (image.withCall ? " +call" : "")
                      << ": linked by DSS, exit " << r.exitCode << "\n";
        }
    }
}
