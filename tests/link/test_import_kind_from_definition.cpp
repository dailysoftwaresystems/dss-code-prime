// ★★★ A REFERENCE THAT STATES NO KIND TAKES IT FROM THE DEFINITION
// (D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL, P68 round 9).
//
// A `.s` address operand, memory displacement or data slot naming a symbol the
// file does not define mints an import whose code-vs-data kind is `Pending`
// (`ExternKindOrigin`; the lowering half is pinned in
// `tests/asm/test_asm_undefined_symbol_reference.cpp`). The kind then comes
// from what DEFINES the name, as it does for ld:
//   * a SIBLING unit's definition binds the reference directly, whatever it is;
//   * a library that states FUNCTION: the reference gets the stub the format's
//     call dispatch provides, the address every DSS reference to that function
//     gets — and since P69 (D-LK-LIBRARY-FUNCTION-ADDRESS-IS-THE-IMAGE-STUB) an
//     ELF executable makes that stub the import's CANONICAL address, the one
//     the rest of the process sees;
//   * a library that states DATUM: refused by name, because the reference would
//     need a copy relocation, which DSS does not make;
//   * a library that states NOTHING (a NOTYPE export): refused by name.
// Nothing is defaulted. ✔MEASURED 2026-09-23 against the references, gcc 13
// with GNU ld 2.42 on x86_64 Linux:
//   * sibling object: gcc -no-pie and -pie both run to 42;
//   * `movq stdout(%rip)`: -no-pie runs to 42 with a copy relocation, and -pie
//     also runs to 42, through a copy relocation of its own;
//   * `leaq puts(%rip)` then a call through it: -no-pie runs to 42, and -pie
//     refuses the PC32 against `puts`;
//   * a NOTYPE export read as a datum: -no-pie and -pie both run to 0, which
//     is WRONG (the datum holds 42). ld copies a symbol of size 0 and warns
//     "type and size of dynamic symbol `nt_sym' are not defined".

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include "import_kind_objects.inc"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

struct Schemas {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

Schemas shipped(std::string_view format) {
    Schemas s;
    auto t = TargetSchema::loadShipped("x86_64");
    EXPECT_TRUE(t.has_value());
    if (t.has_value()) s.target = *t;
    auto f = ObjectFormatSchema::loadShipped(format);
    EXPECT_TRUE(f.has_value()) << "cannot load shipped format " << format;
    if (f.has_value()) s.format = *f;
    return s;
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

constexpr std::uint32_t kImport = 99;

// One unit of assembly: `main` is `leaq <import>(%rip), %rax; ret`, the
// relocation naming `ext`, with the target's RIP-relative kind, as the
// lowering writes it.
AssembledModule referencingUnit(Schemas const& s, std::uint32_t cu, ExternImport ext) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3};
    auto const* rip = s.target->relocationByName("riprel32");
    EXPECT_NE(rip, nullptr);
    fn.relocations.push_back(
        Relocation{3u, ext.symbol, rip != nullptr ? rip->kind : RelocationKind{}, 0});
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    m.externImports.push_back(std::move(ext));
    return m;
}

ExternImport importRow(std::string name, std::string library, ExternKindOrigin origin,
                       bool isData) {
    ExternImport e;
    e.symbol      = SymbolId{kImport};
    e.mangledName = std::move(name);
    e.libraryPath = std::move(library);
    e.kindOrigin  = origin;
    e.isData      = isData;
    return e;
}

}  // namespace

// ── the gate, on an IMAGE ──────────────────────────────────────────────────

TEST(ImportKindFromDefinition, ALibraryThatStatesNoKindIsRefusedByName) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format && s.format->isImageFlavor());
    auto const m = referencingUnit(
        s, 1, importRow("nt_sym", "libnotype.so", ExternKindOrigin::Pending, false));
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    auto const refused = withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable);
    ASSERT_EQ(refused.size(), 1u) << "a kind nothing states must be refused, once";
    EXPECT_TRUE(mentions(refused[0], "'nt_sym'")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "'libnotype.so'")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "reports no kind")) << refused[0].actual;
    EXPECT_FALSE(image.ok());
    EXPECT_EQ(image.resolvedFuncCount, 0u);
}

TEST(ImportKindFromDefinition, ALibraryDatumNamedDirectlyIsRefusedByName) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    auto const m = referencingUnit(
        s, 1, importRow("stdout", "libc.so.6", ExternKindOrigin::FromLibrary, true));
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    auto const refused = withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable);
    ASSERT_EQ(refused.size(), 1u)
        << "a direct reference to a library datum would read its slot";
    EXPECT_TRUE(mentions(refused[0], "'stdout' is a DATUM")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "copy relocation")) << refused[0].actual;
    EXPECT_FALSE(image.ok());
}

// The CONTROL the two refusals need: the same reference, to a library FUNCTION,
// is not refused. A gate that refused every `Pending`-born row would pass both
// tests above and fail this one.
TEST(ImportKindFromDefinition, ALibraryFunctionNamedByAddressIsNotRefused) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    auto const m = referencingUnit(
        s, 1, importRow("puts", "libc.so.6", ExternKindOrigin::FromLibrary, false));
    DiagnosticReporter rep;
    (void)linker::link(m, *s.target, *s.format, rep);
    EXPECT_TRUE(withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable).empty());
}

// A C unit's `extern FILE *stdout;` is `Stated`, and its own references go
// through the loader-filled slot — MIR→LIR says so on the row
// (`readThroughSlot`) — so the gate has nothing to judge. (A `Stated` row whose
// unit names the datum DIRECTLY — every row an object reader mints — IS judged
// since P68 round 11: test_got_slot_lowering.cpp, section C.)
TEST(ImportKindFromDefinition, ACUnitsStatedRowReadThroughItsSlotIsNotJudged) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    auto row = importRow("stdout", "libc.so.6", ExternKindOrigin::Stated, true);
    row.readThroughSlot = true;
    auto const m = referencingUnit(s, 1, std::move(row));
    DiagnosticReporter rep;
    (void)linker::link(m, *s.target, *s.format, rep);
    EXPECT_TRUE(withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable).empty());
}

// An UNBOUND pending row is an undefined symbol, which the reference gate names;
// it is not reported twice under two codes.
TEST(ImportKindFromDefinition, AnUnboundPendingRowIsAnUndefinedSymbol) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    auto const m = referencingUnit(
        s, 1, importRow("nowhere", "", ExternKindOrigin::Pending, false));
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    EXPECT_TRUE(withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable).empty());
    EXPECT_EQ(withCode(rep, DiagnosticCode::K_SymbolUndefined).size(), 1u);
    EXPECT_FALSE(image.ok());
}

// ── a datum is judged by HOW the unit reaches it, whoever stated the kind ──
//
// P69 (D-LK-MEMBER-UNTYPED-EXTERN-TAKEN-AS-DATA): an object reader mints an
// untyped undefined symbol `Pending`, and the archive-member binder takes its
// kind from the library, so a member that reads `stdout` THROUGH THE GOT reaches
// the link as a `FromLibrary` datum. Only a DIRECT code reference to a library
// datum needs a copy relocation (the refusal above): a GOT load reads the
// loader-filled slot, and a data item's pointer is a row against the symbol,
// which the loader fills. Until P69 every `FromLibrary` datum was refused,
// whatever named it, so both of these were refused.

namespace {

// One unit whose `main` reaches `ext` through the target row `kindName` with
// `addend` — in code, or (`fromData`) from a data item's pointer.
AssembledModule unitReaching(Schemas const& s, ExternImport ext, std::string_view kindName,
                             std::int64_t addend, bool fromData) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    auto const* row = s.target->relocationByName(std::string{kindName});
    EXPECT_NE(row, nullptr) << kindName;
    RelocationKind const kind = row != nullptr ? row->kind : RelocationKind{};
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // `movq <ext>(%rip)`-shaped (a load through the field), then `ret`.
    fn.bytes  = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3};
    if (!fromData) fn.relocations.push_back(Relocation{3u, ext.symbol, kind, addend});
    m.functions.push_back(std::move(fn));
    if (fromData) {
        AssembledData d;
        d.symbol    = SymbolId{2};
        d.section   = DataSectionKind::Data;
        d.bytes.assign(8, 0);
        d.alignment = Alignment::ofRuntimePow2(8);
        d.relocations.push_back(Relocation{0u, ext.symbol, kind, addend});
        m.dataItems.push_back(std::move(d));
        m.symbols.push_back(ModuleSymbol{SymbolId{2}, "slot_of_the_datum", SymbolBinding::Global,
                                         SymbolVisibility::Default});
    }
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    m.externImports.push_back(std::move(ext));
    return m;
}

std::string allErrors(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) out += "\n  " + d.actual;
    return out;
}

// ── reading the linked ELF image back (P69 round 4, review-xa3 NIT 10) ────
// What the image ITSELF says: the address `.symtab` gives a function (an
// image's `.symtab` lists its functions), the bytes a loaded address holds, and
// the rows the loader applies.

[[nodiscard]] std::uint64_t le(std::vector<std::uint8_t> const& b, std::uint64_t off, int width) {
    std::uint64_t v = 0;
    for (int i = 0; i < width; ++i) v |= static_cast<std::uint64_t>(b.at(off + i)) << (i * 8);
    return v;
}

struct ElfImageSection {
    std::string   name;
    std::uint32_t type = 0;
    std::uint64_t addr = 0, offset = 0, size = 0;
    std::uint32_t link = 0;
};

[[nodiscard]] std::string cstr(std::vector<std::uint8_t> const& b, std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p) s.push_back(static_cast<char>(b[p]));
    return s;
}

[[nodiscard]] std::vector<ElfImageSection> sectionsOf(std::vector<std::uint8_t> const& b) {
    std::uint64_t const shoff    = le(b, 40, 8);
    auto const          shnum    = static_cast<std::uint16_t>(le(b, 60, 2));
    auto const          shstrndx = static_cast<std::uint16_t>(le(b, 62, 2));
    std::uint64_t const strOff   = le(b, shoff + shstrndx * 64ull + 24, 8);
    std::vector<ElfImageSection> out;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const o = shoff + i * 64ull;
        ElfImageSection s;
        s.name   = cstr(b, strOff + le(b, o, 4));
        s.type   = static_cast<std::uint32_t>(le(b, o + 4, 4));
        s.addr   = le(b, o + 16, 8);
        s.offset = le(b, o + 24, 8);
        s.size   = le(b, o + 32, 8);
        s.link   = static_cast<std::uint32_t>(le(b, o + 40, 4));
        out.push_back(std::move(s));
    }
    return out;
}

constexpr std::uint32_t kShtRela   = 4;
constexpr std::uint32_t kShtNobits = 8;

[[nodiscard]] std::optional<std::uint64_t> functionAddress(std::vector<std::uint8_t> const& b, std::string_view name) {
    auto const secs = sectionsOf(b);
    for (auto const& s : secs) {
        if (s.name != ".symtab" || s.link >= secs.size()) continue;
        auto const& strtab = secs[s.link];
        for (std::uint64_t p = 24; p + 24 <= s.size; p += 24) {
            if (cstr(b, strtab.offset + le(b, s.offset + p, 4)) == name) return le(b, s.offset + p + 8, 8);
        }
    }
    return std::nullopt;
}

// The loaded section holding `va`, if any.
[[nodiscard]] std::optional<ElfImageSection> sectionHolding(std::vector<std::uint8_t> const& b, std::uint64_t va) {
    for (auto const& s : sectionsOf(b)) {
        if (s.addr != 0 && va >= s.addr && va < s.addr + s.size) return s;
    }
    return std::nullopt;
}

// The `width` bytes the image loads at `va` (a NOBITS section's are zero).
[[nodiscard]] std::optional<std::uint64_t> loadedValue(std::vector<std::uint8_t> const& b, std::uint64_t va,
                                                       int width) {
    auto const s = sectionHolding(b, va);
    if (!s.has_value()) return std::nullopt;
    if (s->type == kShtNobits) return 0u;
    return le(b, s->offset + (va - s->addr), width);
}

struct DynamicRow {
    std::uint64_t offset = 0;
    std::uint32_t type   = 0;
    std::string   symbol;
    std::int64_t  addend = 0;
};

// Every RELA row the loader applies (each SHT_RELA section whose symbols are `.dynsym`'s), its symbol named.
[[nodiscard]] std::vector<DynamicRow> dynamicRows(std::vector<std::uint8_t> const& b) {
    auto const secs = sectionsOf(b);
    std::vector<DynamicRow> out;
    for (auto const& s : secs) {
        if (s.type != kShtRela || s.link >= secs.size() || secs[s.link].name != ".dynsym") continue;
        auto const& dynsym = secs[s.link];
        if (dynsym.link >= secs.size()) continue;
        auto const& dynstr = secs[dynsym.link];
        for (std::uint64_t p = 0; p + 24 <= s.size; p += 24) {
            std::uint64_t const info = le(b, s.offset + p + 8, 8);
            DynamicRow r;
            r.offset = le(b, s.offset + p, 8);
            r.type   = static_cast<std::uint32_t>(info & 0xFFFFFFFFu);
            r.addend = static_cast<std::int64_t>(le(b, s.offset + p + 16, 8));
            if (auto const sym = info >> 32; sym != 0) {
                r.symbol = cstr(b, dynstr.offset + le(b, dynsym.offset + sym * 24, 4));
            }
            out.push_back(std::move(r));
        }
    }
    return out;
}

}  // namespace

TEST(ImportKindFromDefinition, ALibraryDatumReadThroughTheGotIsNotRefused) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    // `movq stdout@GOTPCREL(%rip), %rax`: R_X86_64_REX_GOTPCRELX, wire addend -4.
    auto const m = unitReaching(
        s, importRow("stdout", "libc.so.6", ExternKindOrigin::FromLibrary, true),
        "rex_gotpcrelx32", -4, /*fromData=*/false);
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    EXPECT_TRUE(withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable).empty()) << allErrors(rep);
    EXPECT_TRUE(image.ok()) << allErrors(rep);
}

TEST(ImportKindFromDefinition, ALibraryDatumNamedOnlyByADataSlotIsNotRefused) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    // `.quad stdout` in a data item: a row against the symbol, which the loader fills.
    auto const m = unitReaching(
        s, importRow("stdout", "libc.so.6", ExternKindOrigin::FromLibrary, true),
        "abs64", 0, /*fromData=*/true);
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    EXPECT_TRUE(withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable).empty()) << allErrors(rep);
    ASSERT_TRUE(image.ok()) << allErrors(rep);
    // ...and the slot IS that row (review-xa3 NIT 10): ONE loader row fills the data item's slot in `.data`, against
    // `stdout` itself — R_X86_64_64, addend 0 — over a slot the file holds as 0, so what the program reads there is
    // what the loader writes: the datum's address, never the image's own slot for it (an R_X86_64_RELATIVE there
    // would bake in that slot's address). The import's own GOT slot is another row and another section.
    std::vector<DynamicRow> atData;
    for (auto const& r : dynamicRows(image.bytes)) {
        auto const holder = sectionHolding(image.bytes, r.offset);
        if (holder.has_value() && holder->name == ".data") atData.push_back(r);
    }
    ASSERT_EQ(atData.size(), 1u) << "one loader row fills the data item's slot";
    EXPECT_EQ(atData[0].symbol, "stdout") << "against the datum itself";
    EXPECT_EQ(atData[0].type, 1u) << "R_X86_64_64: the loader writes the datum's address";
    EXPECT_EQ(atData[0].addend, 0);
    EXPECT_EQ(loadedValue(image.bytes, atData[0].offset, 8), std::optional<std::uint64_t>{0u})
        << "the slot holds nothing until the loader fills it";
}

// ── a WEAK reference that states no kind, resolved to nothing ─────────────
//
// An image resolves a weak symbol nothing defines to NOTHING, whose value is 0.
// A unit that reads the name through a GOT slot (`cmpq $0, w@GOTPCREL(%rip)`,
// the glibc idiom) reads a slot holding 0 whatever the kind; a DIRECT reference
// computes its field from 0 where the image lets that field reach it — an
// ET_EXEC image sits at its link address, so `leaq w(%rip)` reaches 0 there —
// and is refused by name where it does not (a PIE, which the loader moves).
// Until P69 an object's untyped weak symbol was taken as DATA and resolved to
// the null slot whatever reached it, and then (round 3) refused when named
// directly; the per-field rule is pinned in test_weak_resolved_to_nothing.cpp
// (D-LK-WEAK-UNDEFINED-SYMBOL-NAMED-DIRECTLY-IS-NOT-ADDRESS-ZERO).

TEST(ImportKindFromDefinition, AWeakReferenceThatStatesNoKindReachedThroughTheGotResolvesToNothing) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    auto row = importRow("weak_nobody", "", ExternKindOrigin::Pending, false);
    row.binding = SymbolBinding::Weak;
    auto const m = unitReaching(s, std::move(row), "gotpcrel32", -5, /*fromData=*/false);
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    EXPECT_FALSE(rep.hasErrors()) << allErrors(rep);
    ASSERT_TRUE(image.ok()) << allErrors(rep);
    // ...and the slot it reads holds 0, which no loader row fills (review-xa3 NIT 10). `gotpcrel32` is SLOT + A - P,
    // bias 0, with A = -5 here, so `main`'s patched displacement names the slot at P + disp + 5.
    auto const mainAt = functionAddress(image.bytes, "main");
    ASSERT_TRUE(mainAt.has_value());
    std::uint64_t const field = *mainAt + 3;
    auto const disp = loadedValue(image.bytes, field, 4);
    ASSERT_TRUE(disp.has_value());
    std::uint64_t const slot =
        field + static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(*disp)) + 5);
    auto const holder = sectionHolding(image.bytes, slot);
    ASSERT_TRUE(holder.has_value()) << "the displacement names a slot the image loads";
    EXPECT_NE(holder->name, ".text") << "a slot, not code";
    EXPECT_EQ(loadedValue(image.bytes, slot, 8), std::optional<std::uint64_t>{0u}) << "the slot holds 0";
    for (auto const& r : dynamicRows(image.bytes)) {
        EXPECT_NE(r.offset, slot) << "no loader row fills the weak slot (row type " << r.type << ", '" << r.symbol
                                  << "')";
    }
}

TEST(ImportKindFromDefinition, AWeakReferenceThatStatesNoKindNamedDirectlyReachesZeroInAnExec) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    auto row = importRow("weak_nobody", "", ExternKindOrigin::Pending, false);
    row.binding = SymbolBinding::Weak;
    auto const m = unitReaching(s, std::move(row), "riprel32", 0, /*fromData=*/false);
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    EXPECT_FALSE(rep.hasErrors()) << allErrors(rep);
    EXPECT_TRUE(image.ok()) << allErrors(rep);
}

TEST(ImportKindFromDefinition, AWeakReferenceThatStatesNoKindNamedDirectlyIsRefusedInAPie) {
    auto const s = shipped("elf64-x86_64-linux-pie");
    ASSERT_TRUE(s.target && s.format);
    auto row = importRow("weak_nobody", "", ExternKindOrigin::Pending, false);
    row.binding = SymbolBinding::Weak;
    auto const m = unitReaching(s, std::move(row), "riprel32", 0, /*fromData=*/false);
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    auto const refused = withCode(rep, DiagnosticCode::K_SymbolUndefined);
    ASSERT_EQ(refused.size(), 1u) << allErrors(rep);
    EXPECT_TRUE(mentions(refused[0], "'weak_nobody'")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "names it DIRECTLY")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "a PC-relative field")) << refused[0].actual;
    EXPECT_FALSE(image.ok());
}

// ── a relocatable object keeps the name, as gas does ──────────────────────

TEST(ImportKindFromDefinition, ARelocatableObjectKeepsAPendingReference) {
    auto const s = shipped("elf64-x86_64-linux");
    ASSERT_TRUE(s.target && s.format);
    ASSERT_FALSE(s.format->isImageFlavor());
    auto const m = referencingUnit(
        s, 1, importRow("nt_sym", "libnotype.so", ExternKindOrigin::Pending, false));
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "the linker that consumes the object judges the name, as it does gas's";
    EXPECT_TRUE(image.ok());
    EXPECT_FALSE(image.bytes.empty());
}

// ── a sibling definition decides ──────────────────────────────────────────

TEST(ImportKindFromDefinition, ASiblingDefinitionDecidesAPendingReference) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    std::vector<AssembledModule> mods;
    // Bound to a library that states no kind: alone, that is refused (above).
    mods.push_back(referencingUnit(
        s, 1, importRow("sib_data", "libnotype.so", ExternKindOrigin::Pending, false)));
    AssembledModule sibling;
    sibling.cuId = CompilationUnitId{2};
    sibling.symbols.push_back(ModuleSymbol{SymbolId{7}, "sib_data", SymbolBinding::Global,
                                           SymbolVisibility::Default});
    mods.push_back(std::move(sibling));
    DiagnosticReporter rep;
    auto const image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()}, *s.target, *s.format, rep);
    EXPECT_TRUE(withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable).empty())
        << "the sibling's definition is what the reference binds to";
    ASSERT_EQ(image.resolvedCrossCuRefs.size(), 1u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].reference.cuId.v, 1u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].reference.symbol.v, kImport);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].definition.cuId.v, 2u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].definition.symbol.v, 7u);
}

// ── the merge: a pending row adopts a statement instead of conflicting ────

namespace {

std::size_t conflictsLinking(ExternImport first, ExternImport second) {
    auto const s = shipped("elf64-x86_64-linux");
    std::vector<AssembledModule> mods;
    mods.push_back(referencingUnit(s, 1, std::move(first)));
    auto m2 = referencingUnit(s, 2, std::move(second));
    m2.symbols[0].name = "other";          // one `main`
    m2.userEntrySymbol.reset();
    mods.push_back(std::move(m2));
    DiagnosticReporter rep;
    (void)linker::link(std::span<AssembledModule const>{mods.data(), mods.size()},
                       *s.target, *s.format, rep);
    return withCode(rep, DiagnosticCode::K_ExternImportAttributeConflict).size();
}

}  // namespace

TEST(ImportKindFromDefinition, APendingRowAdoptsTheKindAnotherUnitStates) {
    auto const pending = importRow("x", "libc.so.6", ExternKindOrigin::Pending, false);
    auto const datum   = importRow("x", "libc.so.6", ExternKindOrigin::Stated, true);
    EXPECT_EQ(conflictsLinking(pending, datum), 0u) << "pending first";
    EXPECT_EQ(conflictsLinking(datum, pending), 0u) << "pending second";
}

// The negative the adoption must not swallow: two STATED rows that disagree
// are still a conflict.
TEST(ImportKindFromDefinition, TwoStatementsThatDisagreeStillConflict) {
    auto const code  = importRow("x", "libc.so.6", ExternKindOrigin::Stated, false);
    auto const datum = importRow("x", "libc.so.6", ExternKindOrigin::Stated, true);
    EXPECT_EQ(conflictsLinking(code, datum), 1u);
}

// ── through the driver ────────────────────────────────────────────────────

namespace {

struct Built {
    int                     rc = -1;
    std::vector<ParseDiagnostic> diagnostics;
    std::filesystem::path   out;
};

Built compileAsm(std::filesystem::path const& dir, std::string const& source,
                 std::string const& spec, std::vector<std::filesystem::path> libraries = {},
                 std::string const& language = "asm-x86_64-att") {
    Built b;
    auto const src = dir / "main.s";
    { std::ofstream f{src, std::ios::binary}; f << source; }
    b.out = dir / "out";
    std::filesystem::create_directories(b.out);
    Program p;
    p.setOutputDir(b.out);
    if (!libraries.empty()) p.setResolveLibraries(libraries);
    DiagnosticReporter rep;
    b.rc = p.compileFiles(std::vector<std::string>{src.string()}, language,
                          std::vector<std::string>{spec}, rep);
    b.diagnostics.assign(rep.all().begin(), rep.all().end());
    return b;
}

std::vector<ParseDiagnostic> withCode(Built const& b, DiagnosticCode code) {
    std::vector<ParseDiagnostic> out;
    for (auto const& d : b.diagnostics) {
        if (d.code == code) out.push_back(d);
    }
    return out;
}

constexpr char const* kReadNoKindExport =
    "\t.text\n\t.globl\tmain\n\t.type\tmain, @function\n"
    "main:\n\tmovl\tnt_sym(%rip), %eax\n\tret\n";
constexpr char const* kReadLibraryDatum =
    "\t.text\n\t.globl\tmain\n\t.type\tmain, @function\n"
    "main:\n\tmovq\tstdout(%rip), %rcx\n\tmovl\t$42, %eax\n\tret\n";
// `leaq puts(%rip)`, then a call through that address: the reference that
// states nothing and the one that states code, on one library function.
constexpr char const* kCallThroughLibraryFunctionAddress =
    "\t.text\n\t.globl\tmain\n\t.type\tmain, @function\n"
    "main:\n\tsubq\t$40, %rsp\n\tleaq\tputs(%rip), %rax\n"
    "\tleaq\tmsg(%rip), %rcx\n\tmovq\t%rcx, %rdi\n\tcall\t*%rax\n"
    "\tmovl\t$42, %eax\n\taddq\t$40, %rsp\n\tret\n"
    "\t.data\nmsg:\t.byte\t111, 107, 0\n";

}  // namespace

TEST(ImportKindFromDefinitionProgram, AnAddressOfANoKindLibraryExportIsRefused) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const so = scratch.path() / "libnotype.so";
    {
        auto const bytes = test::notypeSharedLibrary();
        std::ofstream f{so, std::ios::binary};
        f.write(reinterpret_cast<char const*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    auto const b = compileAsm(scratch.path(), kReadNoKindExport,
                              "x86_64:elf64-x86_64-linux-exec", {so});
    EXPECT_NE(b.rc, 0);
    auto const refused = withCode(b, DiagnosticCode::K_ImportReferenceUnbindable);
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_TRUE(mentions(refused[0], "'nt_sym'")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "reports no kind")) << refused[0].actual;
}

TEST(ImportKindFromDefinitionProgram, ADirectReferenceToALibraryDatumIsRefused) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const b = compileAsm(scratch.path(), kReadLibraryDatum,
                              "x86_64:elf64-x86_64-linux-exec");
    EXPECT_NE(b.rc, 0);
    auto const refused = withCode(b, DiagnosticCode::K_ImportReferenceUnbindable);
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_TRUE(mentions(refused[0], "'stdout' is a DATUM")) << refused[0].actual;
}

// The green control through the driver: the definition is CODE, so the address
// is the function's stub, and the program links. Built for ELF on every host;
// the PE twin RUNS where it can.
TEST(ImportKindFromDefinitionProgram, TheAddressOfALibraryFunctionLinks) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const b = compileAsm(scratch.path(), kCallThroughLibraryFunctionAddress,
                              "x86_64:elf64-x86_64-linux-exec");
    std::string why;
    for (auto const& d : b.diagnostics) why += "\n  " + d.actual;
    EXPECT_EQ(b.rc, 0) << why;
    EXPECT_TRUE(std::filesystem::exists(b.out / "main")) << why;
}

TEST(ImportKindFromDefinitionProgram, TheAddressOfALibraryFunctionRunsOnPe) {
#if !defined(_WIN32)
    GTEST_SKIP() << "runs a PE image; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const b = compileAsm(scratch.path(), kCallThroughLibraryFunctionAddress,
                              "x86_64:pe64-x86_64-windows-exec");
    std::string why;
    for (auto const& d : b.diagnostics) why += "\n  " + d.actual;
    ASSERT_EQ(b.rc, 0) << why;
    auto const r = test_support::runBinary(b.out / "main.exe");
    ASSERT_TRUE(r.spawned && !r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 42u);
#endif
}

// The sibling route through the driver: the example's own source and the
// reference-built archive it names. The definitions decide both kinds.
TEST(ImportKindFromDefinitionProgram, ASiblingArchiveDecidesBothKinds) {
    auto const root = test::repoRoot();
    std::string source;
    {
        std::ifstream f{root / "examples/asm/asm_x86_64_address_of_an_undefined_symbol/main.s",
                        std::ios::binary};
        ASSERT_TRUE(f.good());
        source.assign(std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{});
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const elf = compileAsm(scratch.path(), source, "x86_64:elf64-x86_64-linux-exec",
                                {root / "tests/link/data/libasm_sibling_definitions_x86_64_elf.a"});
    std::string why;
    for (auto const& d : elf.diagnostics) why += "\n  " + d.actual;
    EXPECT_EQ(elf.rc, 0) << why;
#if defined(_WIN32)
    test_support::ScratchDir peScratch{test_support::Location::InsideRepo, "import-kind"};
    auto const pe = compileAsm(peScratch.path(), source, "x86_64:pe64-x86_64-windows-exec",
                               {root / "tests/link/data/libasm_sibling_definitions_x86_64_pe.a"});
    why.clear();
    for (auto const& d : pe.diagnostics) why += "\n  " + d.actual;
    ASSERT_EQ(pe.rc, 0) << why;
    auto const r = test_support::runBinary(pe.out / "main.exe");
    ASSERT_TRUE(r.spawned && !r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 42u) << "30 + 10 from the datum, + 2 from the function";
#endif
}

// The arm64 twin: the same lowering, another dialect and another target. Its
// function is never CALLED, so only the sibling's definition states its kind.
// Built on every host; the examples runner runs it under qemu.
TEST(ImportKindFromDefinitionProgram, TheArm64TwinLinksAgainstItsSiblingArchive) {
    auto const root = test::repoRoot();
    std::string source;
    {
        std::ifstream f{root / "examples/asm/asm_arm64_address_of_an_undefined_symbol/main.s",
                        std::ios::binary};
        ASSERT_TRUE(f.good());
        source.assign(std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{});
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const b = compileAsm(scratch.path(), source, "arm64:elf64-aarch64-linux-exec",
                              {root / "tests/link/data/libasm_sibling_definitions_aarch64_elf.a"},
                              "asm-arm64-gas");
    std::string why;
    for (auto const& d : b.diagnostics) why += "\n  " + d.actual;
    EXPECT_EQ(b.rc, 0) << why;
    EXPECT_TRUE(std::filesystem::exists(b.out / "main")) << why;
}
