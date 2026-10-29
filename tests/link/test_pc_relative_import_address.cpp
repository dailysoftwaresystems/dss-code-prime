// P69 review M1 and MINOR 8 (D-LK-PE-IMPORT-ADDRESS-SLOT-RESIDUE): the pieces that give an import's address ONE
// meaning inside every unit of a DSS image, on hand-built modules (every host).
//
//   * `BranchSites` — whether the instruction a PC-relative relocation patches is a branch, read off the TARGET's
//     own declared call/jmp/jcc encodings (x86-64 COFF spells a call and a `lea` with one wire type).
//   * `bindPcRelativeImportAddressUnits` — a unit whose code takes an import's address by a displacement
//     (`leaq puts(%rip)`, MinGW gcc 13.2's DEFAULT `-O2` shape) can only compute the import's CALL ENTRY. On PE
//     (`pcRelativeImportAddress: callEntry`) that unit's every address of the import becomes the call entry — its
//     data, a private clone of the `.refptr` COMDAT other units may be bound to — as link.exe, lld-link and GNU ld
//     make such an object agree with itself; on an ELF shared object and a Mach-O image (`refused`) the reference
//     is refused by name, as GNU ld, lld and Apple's ld refuse it.
//   * `synthesizeLoadTimeFixupRunner` — the function a PE image runs as its first TLS callback to fill the slots its
//     loader cannot bind; its relocations say what it reads and where it writes.
//
// The runs are the examples (`library_function_address_equals_getprocaddress`) and the MinGW-default-shape arm of
// `test_pe_foreign_import_address`.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"   // kTlsIndexReservedSymbolIdValue
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/branch_sites.hpp"
#include "link/load_time_fixups.hpp"
#include "link/object_format_schema.hpp"
#include "link/pc_relative_import_address.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

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
[[nodiscard]] AssembledData const* itemNamed(AssembledModule const& m, SymbolId id) {
    for (auto const& d : m.dataItems) {
        if (d.symbol == id) return &d;
    }
    return nullptr;
}

// One function: `bytes`, with ONE relocation to `target` at `field`.
[[nodiscard]] AssembledFunction function(std::uint32_t symbol, std::vector<std::uint8_t> bytes,
                                         std::uint32_t field, SymbolId target, RelocationKind kind) {
    AssembledFunction fn;
    fn.symbol = SymbolId{symbol};
    fn.bytes  = std::move(bytes);
    fn.relocations.push_back(Relocation{field, target, kind, -4});
    return fn;
}
// One pointer-sized data item holding `target`'s address.
[[nodiscard]] AssembledData pointerTo(std::uint32_t symbol, SymbolId target, RelocationKind abs64,
                                      DataSectionKind section = DataSectionKind::Data) {
    AssembledData d;
    d.symbol    = SymbolId{symbol};
    d.section   = section;
    d.bytes.assign(8, 0);
    d.alignment = Alignment::ofRuntimePow2(8);
    d.relocations.push_back(Relocation{0, target, abs64, 0});
    return d;
}
[[nodiscard]] ExternImport importOf(std::uint32_t symbol, char const* name, char const* lib,
                                    bool isData = false) {
    ExternImport e;
    e.symbol      = SymbolId{symbol};
    e.mangledName = name;
    e.libraryPath = lib;
    e.isData      = isData;
    return e;
}

// x86-64: `lea rax, [rip+d]`, `mov rax, [rip+d]`, `call d`, `jmp d`, `je d`, `call [rip+d]` — the field at the end.
std::vector<std::uint8_t> const kLeaRip   {0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3};
std::vector<std::uint8_t> const kMovRip   {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3};
std::vector<std::uint8_t> const kCallRel  {0xE8, 0, 0, 0, 0, 0xC3};

}  // namespace

// ══ The branch decoder ══════════════════════════════════════════════════════

TEST(BranchSites, TheTargetsOwnBranchEncodingsAreBranchesAndAnAddressComputationIsNot) {
    auto const t = target("x86_64");
    ASSERT_TRUE(t);
    linker::BranchSites const sites{*t};
    ASSERT_FALSE(sites.empty()) << "x86_64 declares call/jmp/jcc encodings";
    struct Case {
        char const*               label;
        std::vector<std::uint8_t> bytes;
        std::uint32_t             field;
        bool                      branch;
    };
    Case const kCases[] = {
        {"call rel32", {0xE8, 0, 0, 0, 0}, 1, true},
        {"jmp rel32", {0xE9, 0, 0, 0, 0}, 1, true},
        {"je rel32", {0x0F, 0x84, 0, 0, 0, 0}, 2, true},
        {"jg rel32 (another condition, one pattern)", {0x0F, 0x8F, 0, 0, 0, 0}, 2, true},
        {"call [rip+d] (a call through memory)", {0xFF, 0x15, 0, 0, 0, 0}, 2, true},
        {"lea rax, [rip+d]", {0x48, 0x8D, 0x05, 0, 0, 0, 0}, 3, false},
        {"mov rax, [rip+d]", {0x48, 0x8B, 0x05, 0, 0, 0, 0}, 3, false},
        {"lea rcx, [rip+d]", {0x48, 0x8D, 0x0D, 0, 0, 0, 0}, 3, false},
        {"a field at the very start reads no opcode", {0, 0, 0, 0}, 0, false},
    };
    for (auto const& c : kCases) {
        EXPECT_EQ(sites.branchEndsAt(c.bytes, c.field), c.branch) << c.label;
    }
}

// ══ The per-unit rule ════════════════════════════════════════════════════════

TEST(PcRelativeImportAddress, APeUnitThatTakesAnImportsAddressByDisplacementBindsEveryAddressOfItToTheCallEntry) {
    auto const t = target("x86_64");
    auto const f = format("pe64-x86_64-windows-exec");
    ASSERT_TRUE(t && f);
    RelocationKind const rel32 = kindNamed(*t, "rel32");      // what the COFF reader gives REL32
    RelocationKind const rip   = kindNamed(*t, "riprel32");
    RelocationKind const abs64 = kindNamed(*t, "abs64");
    // Unit A — MinGW `-O2`: `leaq puts(%rip)`, a `.quad puts`, and a `.refptr.puts`
    // COMDAT another function of the unit loads from.
    AssembledModule a;
    a.cuId = CompilationUnitId{1};
    a.expectedFuncCount = 2;
    a.functions.push_back(function(1, kLeaRip, 3, SymbolId{2}, rel32));
    a.functions.push_back(function(3, kMovRip, 3, SymbolId{6}, rip));
    a.externImports.push_back(importOf(2, "puts", "ucrtbase.dll"));
    a.dataItems.push_back(pointerTo(5, SymbolId{2}, abs64));
    a.dataItems.push_back(pointerTo(6, SymbolId{2}, abs64, DataSectionKind::RelRoConst));
    a.symbols.push_back(ModuleSymbol{SymbolId{1}, "mingw_puts_address", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    a.symbols.push_back(ModuleSymbol{SymbolId{5}, "mingw_table", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    a.symbols.push_back(ModuleSymbol{SymbolId{6}, ".refptr.puts", SymbolBinding::Weak,
                                     SymbolVisibility::Default});
    // Unit B — calls puts and holds `.quad puts`, and takes no address by displacement.
    AssembledModule b;
    b.cuId = CompilationUnitId{2};
    b.expectedFuncCount = 1;
    b.functions.push_back(function(1, kCallRel, 1, SymbolId{2}, rel32));
    b.externImports.push_back(importOf(2, "puts", "ucrtbase.dll"));
    b.dataItems.push_back(pointerTo(4, SymbolId{2}, abs64));
    std::vector<AssembledModule> const units{a, b};
    std::vector<AssembledModule> out;
    DiagnosticReporter rep;
    ASSERT_FALSE(linker::bindPcRelativeImportAddressUnits(units, out, *t, *f, rep)) << "changed";
    ASSERT_FALSE(rep.hasErrors()) << errorText(rep);
    ASSERT_EQ(out.size(), 2u);
    AssembledModule const& ua = out[0];
    SymbolId const entry = ua.externImports[0].callEntrySymbol;
    ASSERT_TRUE(entry.valid()) << "the import gains its call entry as an address";
    EXPECT_EQ(ua.functions[0].relocations[0].target, SymbolId{2})
        << "the lea itself still names the import: it can only reach the call entry anyway";
    ASSERT_NE(itemNamed(ua, SymbolId{5}), nullptr);
    EXPECT_EQ(itemNamed(ua, SymbolId{5})->relocations[0].target, entry)
        << "the unit's `.quad puts` is the call entry too: one value inside the unit";
    ASSERT_NE(itemNamed(ua, SymbolId{6}), nullptr);
    EXPECT_EQ(itemNamed(ua, SymbolId{6})->relocations[0].target, SymbolId{2})
        << "the WEAK `.refptr.puts` is unchanged: other units may be bound to the copy that wins";
    SymbolId const clone = ua.functions[1].relocations[0].target;
    EXPECT_NE(clone, SymbolId{6}) << "the unit loads from a private clone";
    auto const* cloned = itemNamed(ua, clone);
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->relocations[0].target, entry);
    EXPECT_FALSE(cloned->inputSection.has_value()) << "placed on its own, not as a slice of a section";
    for (auto const& ms : ua.symbols) EXPECT_NE(ms.symbol, clone) << "the clone is unit-private: no name";
    // Unit B: untouched.
    AssembledModule const& ub = out[1];
    EXPECT_FALSE(ub.externImports[0].callEntrySymbol.valid());
    EXPECT_EQ(itemNamed(ub, SymbolId{4})->relocations[0].target, SymbolId{2})
        << "a unit that takes no address by displacement keeps the loader-bound address";
}

TEST(PcRelativeImportAddress, ABranchToTheImportChangesNothing) {
    auto const t = target("x86_64");
    auto const f = format("pe64-x86_64-windows-exec");
    ASSERT_TRUE(t && f);
    AssembledModule u;
    u.expectedFuncCount = 1;
    u.functions.push_back(function(1, kCallRel, 1, SymbolId{2}, kindNamed(*t, "rel32")));
    u.externImports.push_back(importOf(2, "puts", "ucrtbase.dll"));
    u.dataItems.push_back(pointerTo(4, SymbolId{2}, kindNamed(*t, "abs64")));
    std::vector<AssembledModule> const units{u};
    std::vector<AssembledModule> out;
    DiagnosticReporter rep;
    EXPECT_TRUE(linker::bindPcRelativeImportAddressUnits(units, out, *t, *f, rep));
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(out.empty());
}

TEST(PcRelativeImportAddress, ASharedObjectRefusesTheUnitByName) {
    // GNU ld and lld: "relocation R_X86_64_PC32 against symbol `puts' can not be used when making a shared
    // object; recompile with -fPIC". A CALL through PLT32 (`isCall`) is a call and passes.
    auto const t = target("x86_64");
    auto const f = format("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(t && f);
    AssembledModule u;
    u.expectedFuncCount = 2;
    u.functions.push_back(function(1, kLeaRip, 3, SymbolId{2}, kindNamed(*t, "riprel32")));
    u.functions.push_back(function(3, kCallRel, 1, SymbolId{2}, kindNamed(*t, "rel32")));
    u.externImports.push_back(importOf(2, "puts", "libc.so.6"));
    u.symbols.push_back(ModuleSymbol{SymbolId{1}, "take_puts", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    std::vector<AssembledModule> const units{u};
    std::vector<AssembledModule> out;
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::bindPcRelativeImportAddressUnits(units, out, *t, *f, rep));
    ASSERT_EQ(rep.errorCount(), 1u) << "one unit, one import: one refusal\n" << errorText(rep);
    std::string const text = errorText(rep);
    EXPECT_NE(text.find("'take_puts'"), std::string::npos) << text;
    EXPECT_NE(text.find("function 'puts' of library 'libc.so.6'"), std::string::npos) << text;
    EXPECT_NE(text.find("PC-relative displacement that is not a branch"), std::string::npos) << text;
    EXPECT_NE(text.find("-fPIC"), std::string::npos) << text;
    bool sawCode = false;
    for (auto const& d : rep.all()) sawCode = sawCode || d.code == DiagnosticCode::K_ImportReferenceUnbindable;
    EXPECT_TRUE(sawCode);
}

TEST(PcRelativeImportAddress, AMachOImageRefusesAnAdrpPairToo) {
    // Apple clang 21's ld: "fixup error (kind=arm64_adrp_lo12) ... target '_puts' does not have address"
    // (measured 2026-10-01). arm64's page relocation patches an instruction WORD: not a branch unless its row says so.
    auto const t = target("arm64");
    auto const f = format("macho64-arm64-darwin-exec");
    ASSERT_TRUE(t && f);
    AssembledModule u;
    u.expectedFuncCount = 1;
    u.functions.push_back(function(1, {0, 0, 0, 0x90, 0xC0, 0x03, 0x5F, 0xD6}, 0, SymbolId{2},
                                   kindNamed(*t, "adr_prel_pg_hi21")));
    u.functions[0].relocations[0].addend = 0;
    u.externImports.push_back(importOf(2, "_puts", "/usr/lib/libSystem.B.dylib"));
    std::vector<AssembledModule> const units{u};
    std::vector<AssembledModule> out;
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::bindPcRelativeImportAddressUnits(units, out, *t, *f, rep));
    EXPECT_EQ(rep.errorCount(), 1u) << errorText(rep);
}

TEST(PcRelativeImportAddress, ADatumAPendingRowAndASlotReadImportAreNotThisRulesQuestion) {
    // A Mach-O image reaches a dylib DATUM through the slot it binds at the datum's own symbol (`lea` + load:
    // `readThroughSlot`), and `FILE **pp = &stdout;` must link. A datum's DIRECT code reference is the copy-relocation
    // case, and a pending row states no kind: both belong to the import-reference judgment that runs after this rule
    // (its own tests). Only the function import here, read directly, is refused.
    auto const t = target("x86_64");
    auto const f = format("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(t && f);
    RelocationKind const rip = kindNamed(*t, "riprel32");
    AssembledModule u;
    u.cuId = CompilationUnitId{1};
    u.expectedFuncCount = 4;
    u.functions.push_back(function(1, kLeaRip, 3, SymbolId{10}, rip));   // a datum, read through its slot
    u.functions.push_back(function(2, kLeaRip, 3, SymbolId{11}, rip));   // a datum named directly
    u.functions.push_back(function(3, kLeaRip, 3, SymbolId{12}, rip));   // a pending row
    u.functions.push_back(function(4, kLeaRip, 3, SymbolId{13}, rip));   // a function, read through a slot
    ExternImport stdoutp = importOf(10, "___stdoutp", "/usr/lib/libSystem.B.dylib", true);
    stdoutp.readThroughSlot = true;
    u.externImports.push_back(stdoutp);
    u.externImports.push_back(importOf(11, "___stderrp", "/usr/lib/libSystem.B.dylib", true));
    ExternImport pending = importOf(12, "_unstated", "/usr/lib/libSystem.B.dylib");
    pending.kindOrigin = ExternKindOrigin::Pending;
    u.externImports.push_back(pending);
    ExternImport slotRead = importOf(13, "_abs", "/usr/lib/libSystem.B.dylib");
    slotRead.readThroughSlot = true;
    u.externImports.push_back(slotRead);
    {
        std::vector<AssembledModule> const units{u};
        std::vector<AssembledModule> out;
        DiagnosticReporter rep;
        EXPECT_TRUE(linker::bindPcRelativeImportAddressUnits(units, out, *t, *f, rep));
        EXPECT_FALSE(rep.hasErrors()) << errorText(rep);
        EXPECT_TRUE(out.empty());
    }
    // The control: the same unit plus one function import its code reaches directly is refused for that one alone.
    u.expectedFuncCount = 5;
    u.functions.push_back(function(5, kLeaRip, 3, SymbolId{14}, rip));
    u.externImports.push_back(importOf(14, "_puts", "/usr/lib/libSystem.B.dylib"));
    std::vector<AssembledModule> const units{u};
    std::vector<AssembledModule> out;
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::bindPcRelativeImportAddressUnits(units, out, *t, *f, rep));
    ASSERT_EQ(rep.errorCount(), 1u) << errorText(rep);
    EXPECT_NE(errorText(rep).find("function '_puts'"), std::string::npos) << errorText(rep);
}

TEST(PcRelativeImportAddress, ANameALinkedUnitDefinesIsNoImport) {
    // The unit's row names `puts`, and another unit DEFINES `puts`: the reference reaches that definition.
    auto const t = target("x86_64");
    auto const f = format("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(t && f);
    AssembledModule u;
    u.cuId = CompilationUnitId{1};
    u.expectedFuncCount = 1;
    u.functions.push_back(function(1, kLeaRip, 3, SymbolId{2}, kindNamed(*t, "riprel32")));
    u.externImports.push_back(importOf(2, "puts", "libc.so.6"));
    AssembledModule d;
    d.cuId = CompilationUnitId{2};
    d.expectedFuncCount = 1;
    AssembledFunction def;
    def.symbol = SymbolId{1};
    def.bytes  = {0xC3};
    d.functions.push_back(def);
    d.symbols.push_back(ModuleSymbol{SymbolId{1}, "puts", SymbolBinding::Global, SymbolVisibility::Default});
    std::vector<AssembledModule> const units{u, d};
    std::vector<AssembledModule> out;
    DiagnosticReporter rep;
    EXPECT_TRUE(linker::bindPcRelativeImportAddressUnits(units, out, *t, *f, rep));
    EXPECT_FALSE(rep.hasErrors()) << errorText(rep);
}

TEST(PcRelativeImportAddress, AnExecutableWhoseStubIsCanonicalDecidesNothing) {
    // ELF executables and PIEs make an address-taken import's stub canonical: the lea is right as written.
    auto const t = target("x86_64");
    ASSERT_TRUE(t);
    for (char const* stem : {"elf64-x86_64-linux-exec", "elf64-x86_64-linux-pie"}) {
        SCOPED_TRACE(stem);
        auto const f = format(stem);
        ASSERT_TRUE(f);
        AssembledModule u;
        u.expectedFuncCount = 1;
        u.functions.push_back(function(1, kLeaRip, 3, SymbolId{2}, kindNamed(*t, "riprel32")));
        u.externImports.push_back(importOf(2, "puts", "libc.so.6"));
        std::vector<AssembledModule> const units{u};
        std::vector<AssembledModule> out;
        DiagnosticReporter rep;
        EXPECT_TRUE(linker::bindPcRelativeImportAddressUnits(units, out, *t, *f, rep));
        EXPECT_FALSE(rep.hasErrors());
    }
}

// ══ The residue runner ═══════════════════════════════════════════════════════

TEST(LoadTimeFixupRunner, ReadsEachSourceSlotAndWritesTheItemAndTheRunningThreadsCopy) {
    auto const t = target("x86_64");
    auto const f = format("pe64-x86_64-windows-exec");
    ASSERT_TRUE(t && f);
    std::vector<linker::LoadTimeFixup> const fixups{
        {SymbolId{20}, 24, SymbolId{30}, 8, false},   // `&arr[3]` of a DLL datum, in an item
        {SymbolId{21}, 0, SymbolId{31}, 0, true},     // `puts` in a thread-local template
    };
    DiagnosticReporter rep;
    auto const fn = linker::synthesizeLoadTimeFixupRunner(fixups, SymbolId{40}, SymbolId{41},
                                                          /*guardArgumentIndex=*/1,
                                                          /*guardValue=*/1, *t, *f, rep);
    ASSERT_TRUE(fn.has_value()) << errorText(rep);
    EXPECT_EQ(fn->symbol, SymbolId{40});
    ASSERT_GE(fn->bytes.size(), 6u);
    // `cmp edx, 1`: the second argument of ms_x64, compared at 32 bits (a DWORD reason).
    EXPECT_EQ(fn->bytes[0], 0x81u);
    EXPECT_EQ(fn->bytes[1], 0xFAu);
    EXPECT_EQ(fn->bytes[2], 0x01u);
    // What it names, in order: source 20, item 30; source 21, the template base 41, item 31 at its
    // thread-block offset, `_tls_index`, item 31 at its thread-block offset again.
    std::vector<std::uint32_t> named;
    for (auto const& r : fn->relocations) named.push_back(r.target.v);
    std::vector<std::uint32_t> const want{20, 30, 21, 41, 31, kTlsIndexReservedSymbolIdValue, 31};
    EXPECT_EQ(named, want);
    RelocationKind const tlsKind = kindNamed(*t, "tls-tpoff32");
    std::size_t tlsRelocs = 0;
    for (auto const& r : fn->relocations) tlsRelocs += r.kind == tlsKind ? 1u : 0u;
    EXPECT_EQ(tlsRelocs, 2u) << "a template item is reached as [base + its offset in a block], twice";
}

TEST(LoadTimeFixupRunner, ATemplateFixupWithNoTemplateBaseIsRefused) {
    auto const t = target("x86_64");
    auto const f = format("pe64-x86_64-windows-exec");
    ASSERT_TRUE(t && f);
    std::vector<linker::LoadTimeFixup> const fixups{{SymbolId{21}, 0, SymbolId{31}, 0, true}};
    DiagnosticReporter rep;
    auto const fn = linker::synthesizeLoadTimeFixupRunner(fixups, SymbolId{40}, SymbolId{},
                                                          1, 1, *t, *f, rep);
    EXPECT_FALSE(fn.has_value());
    EXPECT_NE(errorText(rep).find("no symbol names the template's first byte"), std::string::npos)
        << errorText(rep);
}
