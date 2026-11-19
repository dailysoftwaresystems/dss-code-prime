// ★★ EVERY TAKE SITE OF THE LINK TIER, AT THE EDGE OF ITS ID SPACE (P69, lane `cs`, fold f26d -- main's ruling on the
// take sites, review-csf26c2 N9's class).
//
// THE RULE, per SITE (the door's own pins in tests/link/test_fresh_symbol_ids.cpp cannot see a site): a pass whose
// door has no id left for it refuses BY NAME -- K_SymbolIdSpaceExhausted, naming the site, exactly once -- and what
// its caller holds or receives is exactly as it was: the module the caller passed (its carried end included), and no
// image and no partial module escaping a refusal. A pass that needs several ids on the module ITS CALLER holds takes
// them at once (`ModuleSymbolIds::take(count, ...)`), so it is refused before it touches anything; a pass that takes
// one id at a time does so on its own scratch copy, which its caller discards.
//
// THE TABLE: one row per take site of tests/link/module_door_take_sites.hpp, keyed by its label, each driven through
// the entry its production callers use, with the space raised to the edge through the carried end
// (`symbolIdEnd.keepClearOfHighest`; `room` = the ids left). For every room below the drive's `total` the drive is
// refused, once, by name, with nothing it holds changed and nothing returned; at room `before` the refusal names the
// ROW's site; at room `total` the drive succeeds -- the control, which shows the edge is the only thing refused.
// A multi-take site in place is refused at room k-1 too: one id at a time there would have written k-1 of them.
// The veneer row is its site's one exhaustion cell; the trampoline's site keeps its own cell beside its row,
// LK10EntrySliceC.SymbolIdSpaceExhaustionFailsLoud (tests/link/test_lk10_entry_slice_c.cpp), which pins too that
// the refusal says no K_SymbolUndefined.
//
// THE MERGE'S NUMBERING (`ReportedSymbolIds`, "linker: the merged module") is the one row the table does not drive:
// its space starts at 1, and crossing its edge needs 2^32 - 2 merged symbols, one row each -- hundreds of GiB of
// input (MEASURED by reading: no fixture can hold it, and a start the test could set would be a parameter of the
// public link no real caller passes, which main refused as a test hook). Its door is pinned by
// FreshSymbolIds.ANewSpaceIsNumberedFromOneAndRefusesOnceWhenExhausted, and the path its refusal relies on -- the merge
// reports through the link's reporter and the link refuses on that count before anything is written or returned --
// by ModuleDoorTakeSites.AMergeThatReportsAnErrorReturnsNoImage below.

#include "asm/asm.hpp"
#include "core/types/alignment.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/unit_linker_requests.hpp"
#include "link/branch_veneers.hpp"
#include "link/entry_trampoline.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/pe.hpp"
#include "link/image_request.hpp"
#include "link/import_address_references.hpp"
#include "link/import_call_stub_layout.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link/unit_linker_decisions.hpp"

#include "assembled_module_digest.hpp"
#include "module_door_take_sites.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint32_t kLast = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t kMiB  = 1024u * 1024u;

ImageRequest const kRequest{.artifactFileName = "take_site_probe"};

// The module's space with `room` ids left: its carried end at kLast - room (the door never hands out kLast).
void atRoom(AssembledModule& m, std::uint32_t room) { m.symbolIdEnd.keepClearOfHighest(kLast - room - 1u); }

struct Schemas {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
};
[[nodiscard]] Schemas shipped(std::string_view arch, std::string_view format) {
    Schemas s;
    auto t = TargetSchema::loadShipped(arch);
    EXPECT_TRUE(t.has_value()) << "cannot load shipped target " << arch;
    if (t.has_value()) s.target = *t;
    auto f = ObjectFormatSchema::loadShipped(format);
    EXPECT_TRUE(f.has_value()) << "cannot load shipped format " << format;
    if (f.has_value()) s.format = *f;
    return s;
}

[[nodiscard]] RelocationKind kindNamed(TargetSchema const& t, std::string_view name) {
    auto const* row = t.relocationByName(std::string{name});
    EXPECT_NE(row, nullptr) << name;
    return row != nullptr ? row->kind : RelocationKind{};
}

[[nodiscard]] std::string digestOfAll(std::span<AssembledModule const> units) {
    std::string out;
    for (auto const& u : units) out += test::digestOf(u) + "\n";
    return out;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n    " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n    <no diagnostic>"} : s;
}

// A label names its site in a refusal when each of its literal fragments (split at `{}`) stands in order.
[[nodiscard]] bool namesTheSite(std::string_view actual, std::string_view label) {
    std::size_t at = 0;
    while (true) {
        std::size_t const hole = label.find("{}");
        std::string_view const piece = label.substr(0, hole);
        if (!piece.empty()) {
            std::size_t const found = actual.find(piece, at);
            if (found == std::string_view::npos) return false;
            at = found + piece.size();
        }
        if (hole == std::string_view::npos) return true;
        label.remove_prefix(hole + 2);
    }
}

// What one drive leaves its caller.
// A reporter with no dedup window: the window drops a word-for-word repeat of a recent report, and a site that goes on
// past its refusal repeats it word for word (MEASURED: under the default window the GOT slot's arm survived on
// macos-arm64-debug, run 20261117-084234-b06ffb3c).
[[nodiscard]] DiagnosticReporter::Config noDedupWindow() {
    DiagnosticReporter::Config cfg;
    cfg.dedupWindow = 0;
    return cfg;
}

struct Drive {
    bool               succeeded = false;   // the pass (or the link) said it did its work
    bool               returned  = false;   // an image, or a module, reached the caller
    std::string        heldBefore;          // the digest of everything the caller holds, before ...
    std::string        heldAfter;           // ... and after
    DiagnosticReporter rep{noDedupWindow()};
};
using Driver = std::function<void(std::uint32_t room, Drive& out)>;

struct Row {
    std::string_view label;    // the site's label (tests/link/module_door_take_sites.hpp)
    std::uint32_t    before;   // the ids the drive takes before it reaches this site
    std::uint32_t    total;    // the ids the whole drive takes
    Driver           drive;
};

// ── the fixtures, one per entry a production caller uses ───────────────────────────────────────────────────────

// BRANCH VENEERS (aarch64 ELF exec): an entry whose two calls -- to a function past 144 MiB of `.text` and to its exit
// import's stub -- both need a veneer: two ids, in place.
[[nodiscard]] AssembledFunction word(SymbolId sym, std::uint32_t w) {
    AssembledFunction fn;
    fn.symbol = sym;
    fn.bytes  = {static_cast<std::uint8_t>(w & 0xFFu), static_cast<std::uint8_t>((w >> 8) & 0xFFu),
                 static_cast<std::uint8_t>((w >> 16) & 0xFFu), static_cast<std::uint8_t>((w >> 24) & 0xFFu)};
    return fn;
}
void driveVeneers(std::uint32_t room, Drive& out) {
    static Schemas const s = shipped("arm64", "elf64-aarch64-linux-exec");
    constexpr std::uint32_t kBL = 0x94000000u, kRet = 0xD65F03C0u;
    SymbolId const entry{10}, mainFn{11}, filler{12}, callee{13}, exitImport{50};
    auto const* call26 = s.target->relocationByName("call26");
    ASSERT_NE(call26, nullptr);
    AssembledModule m;
    AssembledFunction e;
    e.symbol = entry;
    for (std::uint32_t const w : {kBL, kBL, kRet}) {
        auto const one = word(entry, w);
        e.bytes.insert(e.bytes.end(), one.bytes.begin(), one.bytes.end());
    }
    e.relocations.push_back(Relocation{0u, callee, call26->kind, 0});
    e.relocations.push_back(Relocation{4u, exitImport, call26->kind, 0});
    m.functions.push_back(std::move(e));
    m.functions.push_back(word(mainFn, kRet));
    AssembledFunction f;
    f.symbol = filler;
    f.bytes.resize(static_cast<std::size_t>(144 * kMiB), 0u);
    m.functions.push_back(std::move(f));
    m.functions.push_back(word(callee, kRet));
    m.expectedFuncCount  = m.functions.size();
    m.imageEntryOverride = 0;
    ExternImport exitImp;
    exitImp.symbol      = exitImport;
    exitImp.mangledName = "exit";
    exitImp.libraryPath = "libc.so.6";
    m.externImports.push_back(exitImp);
    link::ImportCallStubLayout stubs;
    stubs.maxPastTextEnd.emplace(exitImport, 64u);
    atRoom(m, room);
    out.heldBefore = test::digestOf(m);
    out.succeeded  = linker::injectBranchVeneers(m, *s.target, *s.format, stubs, out.rep);
    out.heldAfter  = test::digestOf(m);
}

// THE ENTRY TRAMPOLINE (x86_64 PE exec, a by-name process exit): the trampoline and its exit import, in place.
void driveTrampoline(std::uint32_t room, Drive& out) {
    static Schemas const s = shipped("x86_64", "pe64-x86_64-windows-exec");
    AssembledModule m;
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    m.functions.push_back(std::move(fn));
    m.userEntrySymbol = SymbolId{1};
    atRoom(m, room);
    out.heldBefore = test::digestOf(m);
    out.succeeded  = linker::injectEntryTrampoline(m, *s.target, *s.format, out.rep);
    out.heldAfter  = test::digestOf(m);
}

// THE PE RESIDUE RUNNER (x86_64 PE exec): a datum's address plus 24 in a slot -- its address slot, the runner and
// the runner's thread-template base, on the writer's own copy.
void driveResidue(std::uint32_t room, Drive& out) {
    static Schemas const s = shipped("x86_64", "pe64-x86_64-windows-exec");
    AssembledModule m;
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    m.functions.push_back(std::move(fn));
    AssembledData slot;
    slot.symbol    = SymbolId{5};
    slot.section   = DataSectionKind::Data;
    slot.bytes     = std::vector<std::uint8_t>(24, 0);
    slot.alignment = Alignment::of<8>();
    slot.relocations.push_back(Relocation{8u, SymbolId{99}, kindNamed(*s.target, "abs64"), 24});
    m.dataItems.push_back(std::move(slot));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "_fmode";
    imp.libraryPath = "msvcrt.dll";
    imp.isData      = true;
    m.externImports.push_back(std::move(imp));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_fn", SymbolBinding::Global, SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{5}, "dss_slot", SymbolBinding::Global, SymbolVisibility::Default});
    m.imageEntryOverride = std::size_t{0};   // a writer pin: the image's entry is the module's function
    atRoom(m, room);
    out.heldBefore = test::digestOf(m);
    // `nullopt` is the walker's refusal (fold f26e, B1): no image is returned.
    auto const image = pe::encode(m, *s.target, *s.format, out.rep, kRequest);
    out.succeeded  = image.has_value() && !image->empty();
    out.returned   = image.has_value() && !image->empty();
    out.heldAfter  = test::digestOf(m);
}

// THE IMPORT-ADDRESS FOLD (x86_64 PE exec): `__imp_puts` (an import's address slot) and `__imp_mine` (a linked
// definition's): two rows of one unit, in place. Until this fold the first was rewritten before the second's id was
// asked for, so a refusal at the second left the first rewritten (room 1 is that cell).
void driveImportAddresses(std::uint32_t room, Drive& out) {
    static Schemas const s = shipped("x86_64", "pe64-x86_64-windows-exec");
    auto const unbound = [](std::uint32_t id, std::string name) {
        ExternImport e;
        e.symbol      = SymbolId{id};
        e.mangledName = std::move(name);
        e.isData      = true;
        return e;
    };
    AssembledModule m;
    AssembledFunction fn;
    fn.symbol = SymbolId{3};
    m.functions.push_back(fn);
    m.externImports.push_back(unbound(7, "__imp_puts"));
    m.externImports.push_back(unbound(9, "__imp_mine"));
    std::vector<AssembledModule> mods{m};
    atRoom(mods[0], room);
    std::unordered_set<std::string> const defined{"mine"};
    out.heldBefore = digestOfAll(mods);
    out.succeeded  = linker::foldImportAddressReferences(mods, *s.format, *s.target, defined, out.rep).has_value();
    out.heldAfter  = digestOfAll(mods);
}

// The link's drives: the caller holds its units and receives the image.
void driveLink(std::vector<AssembledModule> const& units, Schemas const& s, Drive& out) {
    out.heldBefore   = digestOfAll(units);
    auto const image = linker::link(std::span<AssembledModule const>{units}, *s.target, *s.format, out.rep, kRequest);
    out.succeeded    = image.ok();
    out.returned     = !image.bytes.empty();
    out.heldAfter    = digestOfAll(units);
}

// A NULL-ADDRESS SYMBOL (x86_64 ELF exec): `main` names a weak `w` nothing defines through a PC-relative field
// (`leaq w(%rip)`), which an ET_EXEC answers with the address 0 -- the reference is retargeted to the row's
// null-address symbol. (An absolute field is written from 0 by the link and takes no symbol: MEASURED, a `.quad w`
// drive reached no null-address take.)
void driveNullAddress(std::uint32_t room, Drive& out) {
    static Schemas const s = shipped("x86_64", "elf64-x86_64-linux-exec");
    AssembledModule m;
    m.cuId              = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x8D, 0x05, 0, 0, 0, 0, 0x31, 0xC0, 0xC3};   // leaq w(%rip), %rax; xor eax, eax; ret
    fn.relocations.push_back(Relocation{3u, SymbolId{99}, kindNamed(*s.target, "riprel32"), 0});
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global, SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    ExternImport w;
    w.symbol      = SymbolId{99};
    w.mangledName = "w";
    w.kindOrigin  = ExternKindOrigin::Pending;
    w.binding     = SymbolBinding::Weak;
    m.externImports.push_back(std::move(w));
    atRoom(m, room);
    driveLink({m}, s, out);
}

// AN OBJECT-CARRIED DATA-IMPORT SLOT (x86_64 PE relocatable): code that reads the data import `ea` through a slot.
void driveObjectImportSlot(std::uint32_t room, Drive& out) {
    static Schemas const s = shipped("x86_64", "pe64-x86_64-windows");
    AssembledModule m;
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes.assign(32u, 0x90u);
    fn.relocations.push_back(Relocation{3u, SymbolId{50}, RelocationKind{1}, 0});
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "usesea", SymbolBinding::Global, SymbolVisibility::Default});
    ExternImport data;
    data.symbol          = SymbolId{50};
    data.mangledName     = "ea";
    data.libraryPath     = "somelib.dll";
    data.isData          = true;
    data.readThroughSlot = s.format->dataImportBinding() == DataImportBinding::GotIndirect;
    m.externImports.push_back(std::move(data));
    atRoom(m, room);
    driveLink({m}, s, out);
}

// A WEAK DEFINITION A COMMON OUTRANKS, in a relocatable artifact (x86_64 ELF): unit 2's weak `shared` gives the name
// up, and its body takes an id of the unit's own.
[[nodiscard]] AssembledFunction readerOf(std::uint32_t id, std::uint32_t target) {
    AssembledFunction fn;
    fn.symbol = SymbolId{id};
    fn.bytes  = {0x8B, 0x05, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(Relocation{2u, SymbolId{target}, RelocationKind{8}, 0});
    return fn;
}
void driveWeakGivesUpItsName(std::uint32_t room, Drive& out) {
    static Schemas const s = shipped("x86_64", "elf64-x86_64-linux");
    AssembledModule withCommon;
    withCommon.cuId              = CompilationUnitId{1};
    withCommon.expectedFuncCount = 1;
    withCommon.functions.push_back(readerOf(1, 2));
    withCommon.symbols.push_back(
        ModuleSymbol{SymbolId{1}, "reader_fn", SymbolBinding::Global, SymbolVisibility::Default});
    ExternImport common;
    common.symbol          = SymbolId{2};
    common.mangledName     = "shared";
    common.isData          = true;
    common.commonSize      = 40;
    common.commonAlignment = 32;
    withCommon.externImports.push_back(std::move(common));
    AssembledModule weak;
    weak.cuId = CompilationUnitId{2};
    AssembledData d;
    d.symbol    = SymbolId{1};
    d.section   = DataSectionKind::Data;
    d.bytes     = {5, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    weak.dataItems.push_back(std::move(d));
    weak.symbols.push_back(ModuleSymbol{SymbolId{1}, "shared", SymbolBinding::Weak, SymbolVisibility::Default});
    weak.functions.push_back(readerOf(2, 1));
    weak.symbols.push_back(ModuleSymbol{SymbolId{2}, "weak_reader", SymbolBinding::Global, SymbolVisibility::Default});
    weak.expectedFuncCount = 1;
    atRoom(weak, room);
    driveLink({withCommon, weak}, s, out);
}

// A LOST COPY'S WEAK NAME THAT STAYS (x86_64 PE relocatable): two units carry a copy of the section group `w`, and
// the later one's copy also carries `m`, a weak name of the kind a PE weak external with a default states
// (overridable, which the document keeps: it does not go with its copy), whose default `.weak.m.ub` is a label of that
// copy -- MinGW-w64 gcc's shape. A third unit defines `m`. The later copy is discarded, and `m` stays as a reference
// row of its unit, under an id of the unit's own. (MEASURED: the carried-alias objects of tests/link/data reach no take
// here, beside a third `m` or not. READ for why: clang-cl's `m` defaults to the group's own name, so it is left to the
// election, which takes no id; gcc's unit reads `m` itself, and a unit that already holds a reference row of the name
// gives that row the default -- INFERRED for gcc's object, whose rows this test does not read.)
[[nodiscard]] AssembledModule copyOfW(std::uint32_t cu, std::uint8_t value) {
    AssembledModule m;
    m.cuId = CompilationUnitId{cu};
    AssembledData d;
    d.symbol       = SymbolId{1};
    d.section      = DataSectionKind::Data;
    d.bytes        = {value, 0, 0, 0};
    d.alignment    = Alignment::of<4>();
    d.inputSection = InputSectionSlice{1u, 0u};
    m.dataItems.push_back(std::move(d));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "w", SymbolBinding::Global, SymbolVisibility::Default});
    m.sectionGroupOfInputSection.emplace(1u, "w");
    return m;
}
void driveLostCopysAlias(std::uint32_t room, Drive& out) {
    static Schemas const s = shipped("x86_64", "pe64-x86_64-windows");
    std::vector<AssembledModule> units;
    units.push_back(copyOfW(1, 7));
    AssembledModule later = copyOfW(2, 9);
    ModuleSymbol m{SymbolId{1}, "m", SymbolBinding::Weak, SymbolVisibility::Default};
    m.weakKind        = WeakDefinitionKind::Overridable;
    m.weakDefaultName = ".weak.m.ub";
    later.symbols.push_back(std::move(m));
    units.push_back(std::move(later));
    AssembledModule strong;   // the third unit: `m`, strong
    strong.cuId = CompilationUnitId{3};
    AssembledData d;
    d.symbol    = SymbolId{1};
    d.section   = DataSectionKind::Data;
    d.bytes     = {1, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    strong.dataItems.push_back(std::move(d));
    strong.symbols.push_back(ModuleSymbol{SymbolId{1}, "m", SymbolBinding::Global, SymbolVisibility::Default});
    units.push_back(std::move(strong));
    atRoom(units[1], room);   // the unit whose copy the link discards
    driveLink(units, s, out);
}

// GOT SLOTS (x86_64 ELF exec): `main` loads two data's addresses through the GOT, two slots. TWO, so a pass that goes
// on past its refused id is seen: with one slot nothing is taken after the refusal, and such a pass returns as one
// that stops (MEASURED: its arm survived on macos-arm64-debug, run 20261117-101609-2e90ea55).
void driveGotSlot(std::uint32_t room, Drive& out) {
    static Schemas const s = shipped("x86_64", "elf64-x86_64-linux-exec");
    AssembledModule m;
    m.cuId              = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // mov rax, [rip + GOT(first)]; mov rcx, [rip + GOT(second)]; xor eax, eax; ret
    fn.bytes = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x31, 0xC0, 0xC3};
    fn.relocations.push_back(Relocation{3u, SymbolId{2}, kindNamed(*s.target, "gotpcrel32"), -4});
    fn.relocations.push_back(Relocation{10u, SymbolId{3}, kindNamed(*s.target, "gotpcrel32"), -4});
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global, SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    for (std::uint32_t const v : {2u, 3u}) {
        AssembledData d;
        d.symbol    = SymbolId{v};
        d.section   = DataSectionKind::Data;
        d.bytes     = std::vector<std::uint8_t>(8, 0);
        d.alignment = Alignment::ofRuntimePow2(8);
        m.dataItems.push_back(std::move(d));
        m.symbols.push_back(ModuleSymbol{SymbolId{v}, v == 2u ? "first" : "second", SymbolBinding::Global,
                                         SymbolVisibility::Default});
    }
    atRoom(m, room);
    driveLink({m}, s, out);
}

// AN IMPORT'S CALL ENTRY (x86_64 PE exec): `main` takes `puts`' address by displacement, so every address of `puts`
// in the unit is its call entry.
void driveCallEntry(std::uint32_t room, Drive& out) {
    static Schemas const s = shipped("x86_64", "pe64-x86_64-windows-exec");
    AssembledModule m;
    m.cuId              = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x8D, 0x05, 0, 0, 0, 0, 0x31, 0xC0, 0xC3};   // lea rax, [rip + puts]; xor eax, eax; ret
    fn.relocations.push_back(Relocation{3u, SymbolId{2}, kindNamed(*s.target, "rel32"), -4});
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global, SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    ExternImport puts;
    puts.symbol      = SymbolId{2};
    puts.mangledName = "puts";
    puts.libraryPath = "ucrtbase.dll";
    m.externImports.push_back(std::move(puts));
    atRoom(m, room);
    driveLink({m}, s, out);
}

// THE ENTRY A UNIT STATES AND DOES NOT DEFINE (`/ENTRY:dss_far_entry`): a required reference row, in place.
void driveRequiredEntry(std::uint32_t room, Drive& out) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{2};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3};
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_internal", SymbolBinding::Global, SymbolVisibility::Default});
    UnitImageRequest entry;
    entry.spelled = " /ENTRY:dss_far_entry";
    entry.value   = UnitEntryRequest{"dss_far_entry"};
    m.linkerRequests.image.push_back(std::move(entry));
    atRoom(m, room);
    out.heldBefore = test::digestOf(m);
    out.succeeded  = linker::requireEntryReference(m, out.rep);
    out.heldAfter  = test::digestOf(m);
}

// A COMPILED UNIT'S LINKER DIRECTIVES (x86_64 PE, P69 fold 3f): `#pragma comment(linker, ...)` asks the link to
// include two functions the unit does not define -- two required reference rows, one id at a time on the pass's own
// copy, which replaces the module only when both were had.
void driveUnitDirectives(std::uint32_t room, Drive& out) {
    static Schemas const s = shipped("x86_64", "pe64-x86_64-windows");
    AssembledModule m;
    m.cuId              = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_fn", SymbolBinding::Global, SymbolVisibility::Default});
    m.linkerRequests.linkerOptions = {{"/INCLUDE:dss_far_a"}, {"/INCLUDE:dss_far_b"}};
    atRoom(m, room);
    out.heldBefore = test::digestOf(m);
    out.succeeded  = pe::applyUnitLinkerDirectives(m, *s.format, out.rep);
    out.heldAfter  = test::digestOf(m);
}

[[nodiscard]] std::vector<Row> const& rows() {
    static std::vector<Row> const kRows = {
        {"branch-veneer: the {} veneer(s)", 0, 2, driveVeneers},
        {"entry-trampoline: the {} id(s) of the trampoline and its exit", 0, 2, driveTrampoline},
        {"pe: a reference row a unit's linker directive asks for", 0, 2, driveUnitDirectives},
        {"pe::encodeExec: the address slot an import-slot fix-up reads", 0, 3, driveResidue},
        {"pe::encodeExec: the import-slot residue runner", 1, 3, driveResidue},
        {"pe::encodeExec: the residue runner's thread-template base", 2, 3, driveResidue},
        {"linker: the {} import-address row(s) of a unit", 0, 2, driveImportAddresses},
        {"linker: a null-address symbol", 0, 3, driveNullAddress},
        {"linker: the object-carried data-import slot for '{}'", 0, 1, driveObjectImportSlot},
        {"linker: the body that leaves its id to the weak name '{}'", 0, 1, driveWeakGivesUpItsName},
        {"linker: the reference row that keeps the weak name '{}'", 0, 1, driveLostCopysAlias},
        {"linker: a GOT slot", 0, 4, driveGotSlot},
        {"linker: an import's call entry a unit is bound to", 0, 3, driveCallEntry},
        {"linker: the reference the entry '{}' requires", 0, 1, driveRequiredEntry},
    };
    return kRows;
}

// The labels a refusal named, in the order the drive met them.
[[nodiscard]] std::vector<std::string> exhaustedIn(DiagnosticReporter const& rep) {
    std::vector<std::string> out;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::K_SymbolIdSpaceExhausted) out.push_back(d.actual);
    return out;
}

}  // namespace

// THE TABLE COVERS THE UNIVERSE: every site of tests/link/module_door_take_sites.hpp is a row (the merge's numbering
// excepted, above), and every row is a site.
TEST(ModuleDoorTakeSites, TheTableHoldsEveryTakeSiteAndNothingElse) {
    std::set<std::string> universe;
    for (auto const& site : test::kLinkTakeSites)
        if (site.label != "linker: the merged module") universe.insert(std::string{site.label});
    std::set<std::string> table;
    for (Row const& r : rows()) table.insert(std::string{r.label});
    EXPECT_EQ(table, universe) << "a take site the table does not drive, or a row that is no take site";
}

// THE RULE, per site and per room below the drive's total; and the control at the total.
TEST(ModuleDoorTakeSites, EveryTakeSiteRefusesItsExhaustionByNameAndLeavesWhatItsCallerHoldsAsItWas) {
    for (Row const& r : rows()) {
        SCOPED_TRACE(std::string{r.label});
        ASSERT_LT(r.before, r.total) << "the row's site is one of its drive's takes";
        for (std::uint32_t room = 0; room < r.total; ++room) {
            SCOPED_TRACE(std::format("{} id(s) left", room));
            Drive d;
            r.drive(room, d);
            auto const named = exhaustedIn(d.rep);
            EXPECT_FALSE(d.succeeded) << "a drive short of ids must be refused" << diagnosticsOf(d.rep);
            EXPECT_FALSE(d.returned) << "a refused drive returns no image";
            EXPECT_EQ(named.size(), 1u) << "refused by name, ONCE" << diagnosticsOf(d.rep);
            EXPECT_EQ(d.rep.errorCount(), 1u)
                << "the refusal is the only error: nothing ran on past it" << diagnosticsOf(d.rep);
            EXPECT_EQ(d.heldAfter, d.heldBefore) << "a refusal leaves what its caller holds as it was";
            if (room == r.before && !named.empty()) {
                EXPECT_TRUE(namesTheSite(named.front(), r.label))
                    << "at " << r.before << " id(s) left the refusal is this row's site's; it said: " << named.front();
            }
        }
        Drive control;
        r.drive(r.total, control);
        EXPECT_TRUE(exhaustedIn(control.rep).empty())
            << "CONTROL: with " << r.total << " id(s) left the drive refuses nothing" << diagnosticsOf(control.rep);
        EXPECT_TRUE(control.succeeded) << "CONTROL: the edge is the only thing refused" << diagnosticsOf(control.rep);
    }
}

// THE MERGE'S REFUSAL PATH. Two units import one `shared` from one library, one as a function and one as a datum: the
// MERGE refuses the pair (K_ExternImportAttributeConflict, reported through the link's reporter), exactly as an
// exhausted numbering would report, and the link returns before anything after the merge runs or is written -- no
// image, and no diagnostic but the merge's. CONTROL: the same two units, agreeing, link.
TEST(ModuleDoorTakeSites, AMergeThatReportsAnErrorReturnsNoImage) {
    Schemas const s = shipped("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    auto const unitsOf = [](bool secondIsData) {
        std::vector<AssembledModule> mods;
        for (std::uint32_t i = 0; i < 2; ++i) {
            AssembledModule m;
            m.cuId              = CompilationUnitId{i + 1};
            m.expectedFuncCount = 1;
            AssembledFunction fn;
            fn.symbol = SymbolId{1};
            fn.bytes  = {0xE8, 0, 0, 0, 0, 0xB8, static_cast<std::uint8_t>(i + 1), 0x00, 0x00, 0x00, 0xC3};
            fn.relocations.push_back(Relocation{1u, SymbolId{2}, RelocationKind{1}, 0});
            m.functions.push_back(std::move(fn));
            ExternImport ext;
            ext.symbol      = SymbolId{2};
            ext.mangledName = "shared";
            ext.libraryPath = "libc.so.6";
            if (i == 1 && secondIsData) {
                ext.isData         = true;
                ext.dataSizeBytes  = 8;
                ext.dataAlignBytes = 8;
            }
            m.externImports.push_back(std::move(ext));
            m.symbols.push_back(ModuleSymbol{SymbolId{1}, std::format("fn{}", i + 1), SymbolBinding::Global,
                                             SymbolVisibility::Default});
            if (i == 0) m.userEntrySymbol = SymbolId{1};
            mods.push_back(std::move(m));
        }
        return mods;
    };
    {
        auto const units = unitsOf(true);
        DiagnosticReporter rep;
        auto const image = linker::link(std::span<AssembledModule const>{units}, *s.target, *s.format, rep, kRequest);
        EXPECT_FALSE(image.ok());
        EXPECT_TRUE(image.bytes.empty()) << "a merge that reported an error writes no image";
        std::size_t merges = 0;
        for (auto const& d : rep.all()) merges += d.code == DiagnosticCode::K_ExternImportAttributeConflict ? 1u : 0u;
        EXPECT_EQ(merges, 1u) << diagnosticsOf(rep);
        EXPECT_EQ(rep.errorCount(), 1u)
            << "nothing after the merge ran: its refusal is the only error" << diagnosticsOf(rep);
    }
    {
        auto const units = unitsOf(false);
        DiagnosticReporter rep;
        auto const image = linker::link(std::span<AssembledModule const>{units}, *s.target, *s.format, rep, kRequest);
        EXPECT_TRUE(image.ok()) << "CONTROL: the same units, agreeing, link" << diagnosticsOf(rep);
    }
}
