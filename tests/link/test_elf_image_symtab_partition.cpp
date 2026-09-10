// The ELF `.symtab`'s TWO LINKAGE AXES — binding and visibility — and the
// local-before-global partition that the binding axis forces.
//
// D-LINK-ELF-IMAGE-STATIC-FN-EMITTED-STB-GLOBAL. Both image `.symtab` builders
// (`elf::encodeElfExecDynamic`, serving the dynamic exec AND every ET_DYN —
// PIE and `.so`; and the static ET_EXEC arm of the shared `elf::encode`)
// stamped STB_GLOBAL on EVERY defined function, so a `static` reached the final
// image under its real name
// (D-LINK-ELF-EXEC-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS) with a linkage that
// misdescribes it, and `.symtab.sh_info` never moved off 2 because nothing
// local ever followed the `.text` section symbol.
//
// D-LK-OBJECT-GLOBAL-HIDDEN-VISIBILITY-EMITTED-LOCAL is the SAME field one
// axis over, and the two are pinned together because one decision produces
// both: `ObjectSymbolNames` used `isExternallyVisible` — a predicate about
// whether another IMAGE may reference the symbol — to answer a question about
// LINKAGE, so a `visibility("hidden")` function (external linkage, invisible
// to other images) came out STB_LOCAL, and at the ET_REL tier lost its NAME to
// the `<prefix><id>` fallback as well. Fixing the binding without the
// visibility would have made a hidden symbol an ordinary export; fixing the
// visibility without the binding leaves it internal-linkage. Hence one file.
//
// ✔MEASURED 2026-09-05 (Ubuntu 24.04, binutils 2.42 / llvm 18), each reference
// probed SEPARATELY, every artifact carrying its own CONTROLS:
//   * gcc 13.3.0 and clang 18.1.3, LINKED `-no-pie` executables, x86_64 and
//     aarch64, -O0 and -O2: a `static` is `FUNC LOCAL DEFAULT` inside the
//     prefix `sh_info` names; a CALLED `visibility("hidden")` function is
//     `FUNC GLOBAL HIDDEN` past it; the CONTROL, an ordinary extern-linkage
//     function of the SAME image, is `FUNC GLOBAL DEFAULT`.
//   * gcc 13.3.0 `-O0 -c`, one object carrying all three visibilities:
//     `GLOBAL HIDDEN` / `GLOBAL INTERNAL` / `GLOBAL PROTECTED`, beside
//     `LOCAL DEFAULT` for a `static` and `GLOBAL DEFAULT` for a plain one.
//   * DSS before this change: every image function STB_GLOBAL with `sh_info`
//     2, and a hidden function `LOCAL` — in the `.o`, under `sym_<id>`.
// The CONTROLS are what make the reading possible at all: without an ordinary
// extern-linkage function in the SAME artifact coming out GLOBAL DEFAULT,
// "gcc emits LOCAL" would be equally consistent with "this toolchain marks
// everything LOCAL".
//
// Both axes are the ONE format-neutral `ObjectSymbolNames` decision — the same
// one the ET_REL writer and both Mach-O tiers read — mapped through the
// file-scope `stbForBinding` / `stvForVisibility`. No ELF-private notion of
// "local": the image tier differs from the object tier ONLY in the NAME it
// prints for a Local (`imageName`).
//
// ★ WHY THE ORDER CHANGES TOO, and why this could not ride on the name fix.
// ELF requires every STB_LOCAL symbol to PRECEDE `sh_info`, so a Local record
// must sort into a local-first prefix or the section header lies about where
// the locals end — and a mis-partitioned `.symtab` is invisible to DSS's own
// reader (which refuses anything but ET_REL and never consults `.symtab`'s
// `sh_info`) while `ld`, `readelf`, `nm` and `gdb` all silently mis-partition.
//
// These pins assert:
//   1. on both ports × all three IMAGE arms: the exact `.symtab` SEQUENCE
//      (names, st_info AND st_other), the exact emitted `sh_info`, and
//      `elfSymtabPartitionBreach` holding over the emitted bytes. Each arm
//      DECLARES the outcome it expects — `sh_info`, whether a `.dynsym` exists
//      and whether that `.dynsym` exports — and the fixture is asserted against
//      the declaration, so a cell cannot silently stop covering the arm it
//      names;
//   2. the ET_REL tier of the SAME module, where the two tiers' one difference
//      (the NAME a Local gets) is the only thing that may differ;
//   2b. the `-staticlib` ARCHIVE INDEX, which is a SECOND statement about the
//      same definitions and was built from the OTHER predicate — an armap that
//      omits a name its own member defines makes a foreign `ld` report
//      "undefined reference" for a symbol the archive contains;
//   3. the same shapes through the REAL pipeline, over corpus sources that are
//      themselves RUN witnesses on the linux x86_64 and aarch64 legs, so the
//      run and this structural pin describe ONE artifact.
//
// ⚠ THE MATRIX CARRIES A **LOCAL CANONICAL WITH A GLOBAL ALIAS** and that row
// is load-bearing rather than decorative. It is the only shape that
// distinguishes the alias pass running INSIDE the binding-ordered walk from it
// running after: with a Global canonical the two emissions are byte-identical.
// It is also REAL rather than synthetic — the object readers produce it every
// time they fold a compiler-private label and an external symbol at one
// address (`tests/link/clang_macho_subsections_object.inc`'s `ltmp0` at the
// n_value of the EXTERNAL `_outer` is exactly it, with the non-external row
// FIRST and therefore canonical).
//
// RED-ON-DISABLE (REMOVE-direction): restore the hardcoded STB_GLOBAL in
// either image builder and (1)/(3) fail on st_info AND on `sh_info`; put
// `isExternallyVisible` back in `ObjectSymbolNames`' shared predicate and the
// hidden rows fail on binding, on st_other AND (at ET_REL) on the name; delete
// the local-first ordering while keeping the binding, or emit the aliases
// inline beside their canonical, and the writer's own
// `elfSymtabPartitionBreach` belt refuses the image with a diagnostic naming
// the anchor.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/binary_readers/ar_reader.hpp"
#include "link/format/elf.hpp"
#include "link/format/elf_symtab_partition.hpp"
#include "link/object_format_schema.hpp"
#include "program/compile_pipeline.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace dss;
using dss::link::format::elfSymtabFirstNonLocal;
using dss::link::format::elfSymtabPartitionBreach;

namespace {

namespace fs = std::filesystem;

// Elf64_Sym st_info values this file asserts, spelled the way the gABI encodes
// them (`bind << 4 | type`) so a reader can check them against `readelf -sW`.
constexpr std::uint8_t kInfoUndef       = 0x00;  // the STN_UNDEF row
constexpr std::uint8_t kInfoLocalSect   = 0x03;  // STB_LOCAL  | STT_SECTION
constexpr std::uint8_t kInfoLocalFunc   = 0x02;  // STB_LOCAL  | STT_FUNC
constexpr std::uint8_t kInfoGlobalFunc  = 0x12;  // STB_GLOBAL | STT_FUNC
constexpr std::uint8_t kInfoWeakFunc    = 0x22;  // STB_WEAK   | STT_FUNC
// Elf64_Sym st_other (gABI 4.18) — the visibility axis, low two bits.
constexpr std::uint8_t kVisDefault      = 0x00;  // STV_DEFAULT
constexpr std::uint8_t kVisHidden       = 0x02;  // STV_HIDDEN

[[nodiscard]] std::uint16_t readU16LE(std::span<std::uint8_t const> b,
                                      std::size_t off) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(b[off])
        | static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[off + 1])
                                     << 8));
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

[[nodiscard]] std::string readCStr(std::vector<std::uint8_t> const& b,
                                   std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p)
        s.push_back(static_cast<char>(b[p]));
    return s;
}

[[nodiscard]] int findSectionByName(std::vector<std::uint8_t> const& b,
                                    std::string const& name) {
    std::uint64_t const shoff    = readU64LE(b, 40);
    std::uint16_t const shnum    = readU16LE(b, 60);
    std::uint16_t const shstrndx = readU16LE(b, 62);
    std::uint64_t const shstrOff = readU64LE(b, shoff + shstrndx * 64 + 24);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint32_t const nameOff = readU32LE(b, shoff + i * 64 + 0);
        if (readCStr(b, shstrOff + nameOff) == name) return static_cast<int>(i);
    }
    return -1;
}

// One Elf64_Sym as this file reads it back: name + BOTH linkage axes, in FILE
// ORDER — the order `sh_info` partitions.
struct SymRecord {
    std::string  name;
    std::uint8_t info  = 0;   // st_info: binding << 4 | type
    std::uint8_t other = 0;   // st_other: visibility
    bool operator==(SymRecord const&) const = default;
};

// A symbol table (`.symtab` or `.dynsym`) read back out of the emitted image,
// together with the boundary its own section header publishes and the raw
// records the predicate reads.
struct SymbolTableView {
    bool                      found = false;
    std::vector<SymRecord>    records;
    std::uint32_t             shInfo = 0;
    std::vector<std::uint8_t> raw;
};

[[nodiscard]] SymbolTableView
readSymbolTable(std::vector<std::uint8_t> const& bytes,
                std::string const& tableName, std::string const& strTableName) {
    SymbolTableView v;
    int const symIdx = findSectionByName(bytes, tableName);
    int const strIdx = findSectionByName(bytes, strTableName);
    if (symIdx < 0 || strIdx < 0) return v;
    std::uint64_t const shoff = readU64LE(bytes, 40);
    std::uint64_t const shdr  = shoff + static_cast<std::uint64_t>(symIdx) * 64;
    std::uint64_t const symOff = readU64LE(bytes, shdr + 24);
    std::uint64_t const symSz  = readU64LE(bytes, shdr + 32);
    std::uint64_t const strOff = readU64LE(
        bytes, shoff + static_cast<std::uint64_t>(strIdx) * 64 + 24);
    v.found  = true;
    v.shInfo = readU32LE(bytes, shdr + 44);
    for (std::uint64_t i = 0; i < symSz / 24; ++i) {
        std::size_t const rec = static_cast<std::size_t>(symOff + i * 24);
        v.records.push_back(
            SymRecord{readCStr(bytes, strOff + readU32LE(bytes, rec)),
                      bytes[rec + 4], bytes[rec + 5]});
    }
    v.raw.assign(bytes.begin() + static_cast<std::ptrdiff_t>(symOff),
                 bytes.begin() + static_cast<std::ptrdiff_t>(symOff + symSz));
    return v;
}

[[nodiscard]] std::vector<std::uint8_t> readFileBytes(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

// Finds one record by name, so the real-pipeline pins can assert about a
// SYMBOL rather than about a position no reference guarantees.
[[nodiscard]] SymRecord const*
findRecord(SymbolTableView const& v, std::string_view name) {
    auto const it = std::find_if(v.records.begin(), v.records.end(),
                                 [&](SymRecord const& r) {
                                     return r.name == name;
                                 });
    return it == v.records.end() ? nullptr : &*it;
}

// The three FINAL-IMAGE arms, named by the builder each selects.
enum class ImageArm { StaticExec, DynamicExec, SharedObject };

struct ElfPortSpec {
    char const*               label;
    char const*               targetName;
    char const*               execFormat;
    char const*               dynFormat;
    char const*               relFormat;
    std::vector<std::uint8_t> retBytes;    // a lone return
    std::vector<std::uint8_t> callBytes;   // call-extern + return
    std::uint32_t             callOffset;  // the reloc patch site
};

std::vector<ElfPortSpec> const kPorts{
    {"x86_64", "x86_64", "elf64-x86_64-linux-exec", "elf64-x86_64-linux-dyn",
     "elf64-x86_64-linux", {0xC3}, {0xE8, 0, 0, 0, 0, 0xC3}, 1},
    {"arm64", "arm64", "elf64-aarch64-linux-exec", "elf64-aarch64-linux-dyn",
     "elf64-aarch64-linux", {0xC0, 0x03, 0x5F, 0xD6},
     {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6}, 0},
};

// The IMAGE `.symtab` each builder must produce, STATED rather than computed.
// Index 0 is STN_UNDEF and index 1 the `.text` STT_SECTION row, both nameless;
// then the LOCAL band (the `static`, then the nameless trampoline-shaped fn #9
// — note this is NOT module order, so the pin sees the REORDER and not merely
// the bit); then the non-local band.
//
// `encodeElfExecDynamic` (the dynamic exec and every ET_DYN) walks
// `module.functions` ONCE PER BAND and emits each canonical's aliases beside
// it inside the non-local pass, so the GLOBAL ALIAS OF THE LOCAL CANONICAL
// OPENS that band — index 4, the first row past `sh_info`. Emitting it inline
// beside its canonical would put it at index 3, INSIDE the prefix `sh_info`
// names, and the writer's own `elfSymtabPartitionBreach` belt would refuse the
// image.
std::vector<SymRecord> const kExpectedDynamicImageSymtab{
    {"",                  kInfoUndef,      kVisDefault},
    {"",                  kInfoLocalSect,  kVisDefault},
    {"img_static_fn",     kInfoLocalFunc,  kVisDefault},
    {"sym_9",             kInfoLocalFunc,  kVisDefault},
    {"img_static_alias",  kInfoGlobalFunc, kVisDefault},
    {"img_global_fn",     kInfoGlobalFunc, kVisDefault},
    {"img_weak_alias",    kInfoWeakFunc,   kVisDefault},
    {"img_hidden_fn",     kInfoGlobalFunc, kVisHidden},
    {"img_hidden_alias",  kInfoGlobalFunc, kVisHidden},
};

// The static ET_EXEC arm is the `isExec` path of the SHARED `elf::encode`,
// which runs ONE alias pass at the END over `aliasSites` (registered in
// emission order 7, 9, 8, 10) rather than in band. All three aliases therefore
// follow all three non-local canonicals. Same bands, same `sh_info`, different
// interleave — and both are correct, because ELF constrains the PARTITION and
// not the order within a band.
std::vector<SymRecord> const kExpectedStaticImageSymtab{
    {"",                  kInfoUndef,      kVisDefault},
    {"",                  kInfoLocalSect,  kVisDefault},
    {"img_static_fn",     kInfoLocalFunc,  kVisDefault},
    {"sym_9",             kInfoLocalFunc,  kVisDefault},
    {"img_global_fn",     kInfoGlobalFunc, kVisDefault},
    {"img_hidden_fn",     kInfoGlobalFunc, kVisHidden},
    {"img_static_alias",  kInfoGlobalFunc, kVisDefault},
    {"img_weak_alias",    kInfoWeakFunc,   kVisDefault},
    {"img_hidden_alias",  kInfoGlobalFunc, kVisHidden},
};

// ⚠⚠ STATED PER ARM, NEVER DERIVED FROM THE FIXTURE, and that is the whole
// point of this table. A cell that computed `expectsDynsym` from the bytes it
// just read would agree with the fixture whichever way the fixture went — and
// the failure mode is not hypothetical: the static ET_EXEC arm is reachable
// ONLY with zero extern imports, so the day a shipped exec document (or the
// dispatch) routed a zero-extern module through the dynamic builder, BOTH exec
// cells would have been exercising ONE builder while still passing every
// sequence assertion below. Stating the expectation and asserting the fixture
// against it turns that silent emptying into a red that names the arm that
// moved.
struct ArmSpec {
    ImageArm      arm;
    char const*   label;
    bool          expectsDynsym;      // the dynamic builder's marker section
    bool          expectsDynExports;  // ET_DYN publishes an export set
    std::uint32_t expectedShInfo;     // the local prefix's length
    // ★ THE SEQUENCE IS PER ARM, because the two builders legitimately
    // INTERLEAVE the alias rows differently — and until this fixture grew a
    // Local canonical WITH a Global alias, the two sequences were identical
    // and one table served both. That coincidence is exactly what made the
    // alias-placement claim untestable: with only a Global canonical carrying
    // an alias, `elf::encode`'s end-of-table alias pass and
    // `encodeElfExecDynamic`'s in-band one land the row in the same place.
    std::vector<SymRecord> const* expectedSymtab;
};

std::vector<ArmSpec> const kArms{
    {ImageArm::StaticExec, " [static ET_EXEC arm]",
     /*expectsDynsym=*/false, /*expectsDynExports=*/false,
     /*expectedShInfo=*/4, &kExpectedStaticImageSymtab},
    {ImageArm::DynamicExec, " [dynamic exec arm]",
     /*expectsDynsym=*/true, /*expectsDynExports=*/false,
     /*expectedShInfo=*/4, &kExpectedDynamicImageSymtab},
    {ImageArm::SharedObject, " [ET_DYN shared-object arm]",
     /*expectsDynsym=*/true, /*expectsDynExports=*/true,
     /*expectedShInfo=*/4, &kExpectedDynamicImageSymtab},
};

// The ET_REL `.symtab` for the SAME module. Two differences, both required:
//   * a Local canonical takes the `<prefix><id>` fallback NAME here, because
//     these names ARE a foreign linker's resolution keys and a real-named
//     static collides across TUs (`definedName` vs `imageName`);
//   * the alias pass runs once at the END over `aliasSites` rather than
//     interleaved, so both aliases follow both global canonicals.
// The hidden row keeps its REAL name on BOTH tiers — that is the whole of
// D-LK-OBJECT-GLOBAL-HIDDEN-VISIBILITY-EMITTED-LOCAL's name half, and gcc's
// own `.o` says `GLOBAL HIDDEN vis_lead`, never `LOCAL sym_<n>`.
std::vector<SymRecord> const kExpectedRelSymtab{
    {"",                  kInfoUndef,      kVisDefault},
    {"",                  kInfoLocalSect,  kVisDefault},
    {"sym_7",             kInfoLocalFunc,  kVisDefault},
    {"sym_9",             kInfoLocalFunc,  kVisDefault},
    {"img_global_fn",     kInfoGlobalFunc, kVisDefault},
    {"img_hidden_fn",     kInfoGlobalFunc, kVisHidden},
    {"img_static_alias",  kInfoGlobalFunc, kVisDefault},
    {"img_weak_alias",    kInfoWeakFunc,   kVisDefault},
    {"img_hidden_alias",  kInfoGlobalFunc, kVisHidden},
};

// The fixture module both tiers encode.
//   fn #7  — a `static` (a Local row WITH a declared name): THE binding case.
//            It ALSO carries a GLOBAL alias, which is the only shape that can
//            tell an interleaved alias emission from a banded one.
//   fn #8  — externally visible, plus a second (WEAK) name.
//   fn #9  — NO `ModuleSymbol` row: the linker-injected trampoline's shape,
//            which the shared predicate resolves to Local as well.
//   fn #10 — Global binding + HIDDEN visibility: THE visibility case. It ALSO
//            carries a Global+HIDDEN ALIAS, and that row is the ALIAS SET'S
//            OWN widening — see below.
// Module order 7, 8, 9, 10 — so the local band (7, 9) is NOT a prefix of it.
//
// ⚠⚠ `img_hidden_alias` PINS THE HALF OF THIS CHANGE THAT IS *NOT* CONFINED TO
// THE `.o` TIER. Swapping `definedAliases`' gate from `isExternallyVisible` to
// `hasExternalLinkage` did not merely restore a name: it WIDENED the alias set
// itself, admitting every Global/Weak + Hidden/Internal extra name that the
// old predicate dropped. A widened alias set adds a RECORD, and a record moves
// every count downstream of it — the ELF non-local band's length here, and on
// Mach-O `numLocals` / `iextdefsym` / `iundefsym` plus every `__stubs`/`__got`
// indirect index computed as `numDefs + <extern index>`. The byte-identity
// control taken over the corpus CANNOT see that: no shipped corpus source
// carries an alias at all, because the c front end has no `alias` attribute.
// The shape IS reachable, through the READERS rather than through C — a
// foreign `.o` carrying `__attribute__((alias("x"), visibility("hidden")))`
// produces it, since `elf_object_reader.cpp`'s `stvToVisibility` lifts
// STV_HIDDEN alongside STB_GLOBAL and `macho_object_reader.cpp` maps a
// `private external` at an atom boundary to Global+Hidden. So the widening is
// pinned HERE, at the writer, on both ports and on every arm. ⚠ The Mach-O
// half of the same widening is NOT pinned this cycle: that writer is held by a
// concurrent lane and this lane was refused it.
void buildFixtureModule(AssembledModule& mod, ElfPortSpec const& port,
                        bool wantsExtern) {
    mod.expectedFuncCount = 4;
    for (std::uint32_t id : {7u, 8u, 9u, 10u}) {
        AssembledFunction f;
        f.symbol = SymbolId{id};
        f.bytes  = (id == 7u && wantsExtern) ? port.callBytes : port.retBytes;
        if (id == 7u && wantsExtern) {
            Relocation rel;
            rel.offset = port.callOffset;
            rel.target = SymbolId{99};
            rel.kind   = RelocationKind{1};   // call rel32 / call26
            f.relocations.push_back(rel);
        }
        mod.functions.push_back(std::move(f));
    }
    mod.symbols.push_back(ModuleSymbol{SymbolId{7}, "img_static_fn",
                                       SymbolBinding::Local,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{7}, "img_static_alias",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{8}, "img_global_fn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{8}, "img_weak_alias",
                                       SymbolBinding::Weak,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "img_hidden_fn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Hidden});
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "img_hidden_alias",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Hidden});
    if (wantsExtern) {
        mod.externImports.push_back(
            ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    }
}

} // namespace

// ── (1) THE IMAGE MATRIX: both ports × all three image arms ─────────────────

TEST(ElfImageSymtabPartition,
     StaticFunctionIsLocalAndSortsFirstOnEveryImageArm) {
    // ONE cell = one (port, arm) pair. `void` on purpose: a failed ASSERT
    // returns from the BODY instead of aborting the whole matrix, which is how
    // a sibling pin once ran only its x86_64 half.
    auto runCell = [](ElfPortSpec const& port, ArmSpec const& armSpec) -> void {
        std::string const label = std::string{port.label} + armSpec.label;
        bool const isDynObject  = armSpec.arm == ImageArm::SharedObject;
        bool const wantsExtern  = armSpec.arm == ImageArm::DynamicExec;

        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto fmt = ObjectFormatSchema::loadShipped(
            isDynObject ? port.dynFormat : port.execFormat);
        ASSERT_TRUE(fmt.has_value()) << label;
        ASSERT_NE(*fmt, nullptr) << label;

        AssembledModule mod;
        buildFixtureModule(mod, port, wantsExtern);
        if (!isDynObject) mod.imageEntryOverride = std::size_t{0};

        DiagnosticReporter rep;
        auto const bytes = elf::encode(mod, **target, **fmt, rep);
        std::string diags;
        for (auto const& d : rep.all()) diags += "\n  " + d.actual;
        ASSERT_EQ(rep.errorCount(), 0u) << label << diags;
        ASSERT_FALSE(bytes.empty()) << label << diags;

        // Each cell must reach the arm it NAMES — see `ArmSpec`'s comment.
        EXPECT_EQ(findSectionByName(bytes, ".dynsym") >= 0,
                  armSpec.expectsDynsym)
            << label
            << ": this cell did not reach the image builder it names, so its "
               "result says nothing about that builder";

        auto const symtab = readSymbolTable(bytes, ".symtab", ".strtab");
        ASSERT_TRUE(symtab.found) << label << ": no `.symtab` in the image";

        // THE FIX, as an exact sequence: the `static` is STB_LOCAL, it sorts
        // into the local prefix ahead of the externally visible definition it
        // FOLLOWS in module order, its GLOBAL alias sorts PAST the boundary,
        // the weak alias keeps its own binding, and the hidden function keeps
        // STB_GLOBAL while stating STV_HIDDEN in st_other.
        EXPECT_EQ(symtab.records, *armSpec.expectedSymtab)
            << label
            << ": the image `.symtab` must be the local band (the `static` and "
               "the nameless trampoline-shaped symbol) followed by the "
               "non-local band, which must CONTAIN the Global alias of the "
               "Local canonical rather than emit it beside that canonical";

        // The boundary the section header PUBLISHES, against the one this arm
        // DECLARES. `armSpec.expectedShInfo` is the load-bearing comparison —
        // it is an absolute the fixture cannot move.
        EXPECT_EQ(symtab.shInfo, armSpec.expectedShInfo)
            << label << ": `.symtab.sh_info` must name the first non-LOCAL "
                        "symbol";
        // ...and the same absolute, re-derived from the emitted st_info bytes
        // by the predicate the WRITER uses, so a writer that published a
        // boundary its own records disagree with is caught here too. (This one
        // shares its implementation with the writer, so it is a consistency
        // check, not a second independent source — `expectedShInfo` above is
        // the independent one.)
        EXPECT_EQ(elfSymtabFirstNonLocal(symtab.raw), armSpec.expectedShInfo)
            << label
            << ": the boundary read back off the records must be the one this "
               "arm declares";
        EXPECT_EQ(elfSymtabPartitionBreach(symtab.raw, symtab.shInfo), "")
            << label << ": the emitted `.symtab` must satisfy ELF's "
                        "local-before-global partition";

        // `.dynsym` obeys the same rule, and its export set is where a
        // `static` — or a HIDDEN symbol — leaking would be a real ABI defect
        // rather than a description defect. The ET_DYN arm is the only one
        // that exports.
        if (armSpec.expectsDynsym) {
            auto const dynsym = readSymbolTable(bytes, ".dynsym", ".dynstr");
            ASSERT_TRUE(dynsym.found) << label;
            EXPECT_EQ(elfSymtabFirstNonLocal(dynsym.raw), dynsym.shInfo)
                << label << ": `.dynsym.sh_info` must name its first non-LOCAL";
            EXPECT_EQ(elfSymtabPartitionBreach(dynsym.raw, dynsym.shInfo), "")
                << label;
            auto hasDynName = [&](std::string_view want) {
                return findRecord(dynsym, want) != nullptr;
            };
            EXPECT_EQ(hasDynName("img_global_fn"), armSpec.expectsDynExports)
                << label
                << ": whether this arm publishes an export set is declared by "
                   "the arm, not read off the fixture";
            EXPECT_FALSE(hasDynName("img_static_fn"))
                << label
                << ": a `static` must never reach the dynamic export set — "
                   "that would be an ABI leak, not merely a wrong description";
            EXPECT_FALSE(hasDynName("img_hidden_fn"))
                << label
                << ": nor may a `visibility(\"hidden\")` symbol — external "
                   "LINKAGE is not dynamic VISIBILITY, and the export gate is "
                   "the one place `isExternallyVisible` is still the right "
                   "question";
            EXPECT_FALSE(hasDynName("img_hidden_alias"))
                << label
                << ": and a HIDDEN ALIAS is gated by the same question as its "
                   "canonical — the widened `definedAliases` set reaches the "
                   "`.symtab`, never the dynamic export set";
        }
    };

    for (auto const& port : kPorts)
        for (auto const& arm : kArms) runCell(port, arm);
}

// ── (2) THE ET_REL TIER of the SAME module ─────────────────────────────────

TEST(ElfImageSymtabPartition, RelocatableTierKeepsBothAxesAndCarvesOnlyNames) {
    auto runCell = [](ElfPortSpec const& port) -> void {
        std::string const label = std::string{port.label} + " [ET_REL]";
        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto fmt = ObjectFormatSchema::loadShipped(port.relFormat);
        ASSERT_TRUE(fmt.has_value()) << label;
        ASSERT_NE(*fmt, nullptr) << label;

        AssembledModule mod;
        buildFixtureModule(mod, port, /*wantsExtern=*/false);

        DiagnosticReporter rep;
        auto const bytes = elf::encode(mod, **target, **fmt, rep);
        std::string diags;
        for (auto const& d : rep.all()) diags += "\n  " + d.actual;
        ASSERT_EQ(rep.errorCount(), 0u) << label << diags;
        ASSERT_FALSE(bytes.empty()) << label << diags;

        auto const symtab = readSymbolTable(bytes, ".symtab", ".strtab");
        ASSERT_TRUE(symtab.found) << label;
        EXPECT_EQ(symtab.records, kExpectedRelSymtab)
            << label
            << ": the `.o` carves a LOCAL's name to `sym_<id>` and nothing "
               "else — a hidden symbol keeps its real name, STB_GLOBAL and "
               "STV_HIDDEN, which is what gcc and clang both emit";
        EXPECT_EQ(symtab.shInfo, 4u)
            << label << ": `.symtab.sh_info` must name the first non-LOCAL";
        EXPECT_EQ(elfSymtabPartitionBreach(symtab.raw, symtab.shInfo), "")
            << label;
    };
    for (auto const& port : kPorts) runCell(port);
}

// ── (2b) THE THIRD TIER: a `-staticlib` ARCHIVE'S OWN INDEX ────────────────
//
// D-LK-OBJECT-GLOBAL-HIDDEN-VISIBILITY-EMITTED-LOCAL, the half that is NOT a
// symbol table. An `ar` archive carries a SECOND statement about its members'
// definitions — the "/" armap, which is the index a foreign linker actually
// searches — and it was built from a DIFFERENT predicate from the one the
// member object's `.symtab` used. The member `.o` names an external-linkage
// definition through `definedName`/`hasExternalLinkage`; the index filtered by
// `isExternallyVisible`, which folds visibility in. So after the name/binding
// fix a `dsscp --target x86_64:elf64-x86_64-linux-staticlib` build over a
// source with a `visibility("hidden")` non-static function shipped a `.a` that
// CONTRADICTED ITSELF: the member defines the symbol under its real name with
// STB_GLOBAL and the index omits it, so a foreign `ld` reports "undefined
// reference" for exactly the symbol this anchor exists to make resolvable.
// Reachable from the CLI, not from the shipped corpus (no example builds a
// `-staticlib` over a hidden definition), which is why the witness is here.
//
// ✔MEASURED 2026-09-05, each reference probed SEPARATELY, CONTROLS inside the
// SAME member object: gcc 13.3.0 with GNU ar/nm 2.42 at -O0 and -O2, and clang
// 18.1.3, ALL list `visibility("hidden")`, `("internal")` AND `("protected")`
// functions in the armap and omit the `static`; `gcc use.o libhid.a` then
// links rc 0 and RUNS to 42 resolving the hidden name out of the archive,
// while the CONTROL link that asks for the `static` name fails "undefined
// reference to `static_local'". Without those CONTROLS "hidden is indexed"
// would be equally consistent with "GNU ar indexes everything".
TEST(ElfHiddenVisibility, StaticArchiveIndexAgreesWithTheMemberItIndexes) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());
    ASSERT_NE(*fmt, nullptr);

    // One member object carrying every visibility beside BOTH controls — the
    // shape the reference measurement used, so the two are comparable.
    struct Def {
        char const*      name;
        SymbolBinding    binding;
        SymbolVisibility visibility;
        bool             mustBeIndexed;
    };
    std::vector<Def> const defs{
        {"ar_hidden",    SymbolBinding::Global, SymbolVisibility::Hidden,    true},
        {"ar_internal",  SymbolBinding::Global, SymbolVisibility::Internal,  true},
        {"ar_protected", SymbolBinding::Global, SymbolVisibility::Protected, true},
        {"ar_weak",      SymbolBinding::Weak,   SymbolVisibility::Hidden,    true},
        // CONTROL 1 — a `static`. Internal linkage: no other TU may name it,
        // so it must NOT be in the index. GNU `ar` agrees, and a link that
        // asks for it fails "undefined reference".
        {"ar_static",    SymbolBinding::Local,  SymbolVisibility::Default,   false},
        // CONTROL 2 — an ordinary extern-linkage function. It must be
        // PRESENT; without it "the hidden names are indexed" would be equally
        // consistent with "this writer indexes nothing at all".
        {"ar_plain",     SymbolBinding::Global, SymbolVisibility::Default,   true},
    };

    AssembledModule mod;
    mod.expectedFuncCount = defs.size();
    for (std::size_t i = 0; i < defs.size(); ++i) {
        AssembledFunction fn;
        fn.symbol = SymbolId{static_cast<std::uint32_t>(20 + i)};
        fn.bytes  = {0xC3};   // ret
        mod.functions.push_back(std::move(fn));
        mod.symbols.push_back(
            ModuleSymbol{SymbolId{static_cast<std::uint32_t>(20 + i)},
                         defs[i].name, defs[i].binding, defs[i].visibility});
    }

    std::string const memberName = "libhid.o";
    auto const        outPath    = fs::temp_directory_path()
                                / "dss_p61_ef_hidden_armap.a";
    std::error_code ec;
    fs::remove(outPath, ec);

    DiagnosticReporter rep;
    ASSERT_TRUE(linkAndWriteStaticArchive(
        std::span<AssembledModule const>{&mod, 1},
        std::span<std::string const>{&memberName, 1},
        **target, **fmt, outPath, rep))
        << "linkAndWriteStaticArchive failed; errs=" << rep.errorCount();

    auto const bytes = readFileBytes(outPath);
    fs::remove(outPath, ec);
    ASSERT_FALSE(bytes.empty());

    DiagnosticReporter rrep;
    auto const archive = ffi::readArArchive(bytes, "libhid.a", rrep);
    ASSERT_TRUE(archive.has_value()) << archive.error().detail;

    auto indexed = [&](std::string_view want) {
        return std::any_of(archive->symbols.begin(), archive->symbols.end(),
                           [&](auto const& s) { return s.name == want; });
    };
    for (Def const& d : defs) {
        EXPECT_EQ(indexed(d.name), d.mustBeIndexed)
            << d.name
            << ": the archive INDEX must list exactly the definitions the "
               "member object names in its own `.symtab` — an index that "
               "disagrees with its member makes a foreign `ld` report "
               "\"undefined reference\" for a symbol the archive contains";
    }

    // ...and the index must agree with the MEMBER, read back from the archive
    // rather than assumed: whatever the `.o` gave a real name and a non-LOCAL
    // binding is exactly what the armap must carry. This is the comparison the
    // two predicates drifting apart breaks, and it needs no fixture constant.
    ASSERT_EQ(archive->members.size(), 1u);
    auto const& member = archive->members[0];
    std::span<std::uint8_t const> const memberBytes{
        bytes.data() + static_cast<std::size_t>(member.dataOffset),
        static_cast<std::size_t>(member.size)};
    std::vector<std::uint8_t> const memberObj{memberBytes.begin(),
                                              memberBytes.end()};
    auto const memberSymtab = readSymbolTable(memberObj, ".symtab", ".strtab");
    ASSERT_TRUE(memberSymtab.found) << "no `.symtab` in the archived member";
    for (SymRecord const& r : memberSymtab.records) {
        if (r.name.empty() || r.name.rfind("sym_", 0) == 0) continue;
        if ((r.info >> 4) == 0) continue;   // STB_LOCAL — never an armap row
        EXPECT_TRUE(indexed(r.name))
            << r.name
            << ": the member's `.symtab` names this definition with a "
               "non-LOCAL binding, so the archive index must too";
    }
}

// ── (2c) THE RESOLVER THAT READS THAT INDEX ────────────────────────────────
//
// The armap fix above is only half a fix, and the other half is a hazard the
// fix ITSELF creates — D-LK-OBJECT-GLOBAL-HIDDEN-VISIBILITY-EMITTED-LOCAL.
// `pullStaticArchiveMembers` decides which archive members to pull by
// subtracting the names ALREADY defined by the modules in the link. That set
// was built with `isExternallyVisible` too, so a hidden definition already in
// the link did not count as satisfying anything. While the armap ALSO omitted
// hidden names the two agreed by accident and nothing was pulled. The moment
// the index starts listing them, a resolver that still disagrees pulls a
// member defining a symbol the link already has — two definitions of one
// symbol, produced by fixing the writer alone. Both sides now ask
// `hasExternalLinkage`.
//
// ✔MEASURED, and this is why the widened index is right in the first place: a
// hidden definition inside an archive member DOES satisfy a cross-module
// reference — `gcc use.o libhid.a -o prog` (gcc 13.3.0 / binutils 2.42, and
// clang 18.1.3) links rc 0 and RUNS to 42 against an archive whose only
// definition of the referenced symbol is `visibility("hidden")`.
TEST(ElfHiddenVisibility, StaticPullTreatsAHiddenDefinitionAsSatisfying) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());
    ASSERT_NE(*fmt, nullptr);

    // The archive: one member defining `pull_hidden` with HIDDEN visibility.
    AssembledModule libMod;
    libMod.expectedFuncCount = 1;
    AssembledFunction libFn;
    libFn.symbol = SymbolId{40};
    libFn.bytes  = {0xC3};
    libMod.functions.push_back(std::move(libFn));
    libMod.symbols.push_back(ModuleSymbol{SymbolId{40}, "pull_hidden",
                                          SymbolBinding::Global,
                                          SymbolVisibility::Hidden});

    std::string const memberName = "pullhid.o";
    auto const        archivePath =
        fs::temp_directory_path() / "dss_p61_ef_hidden_pull.a";
    std::error_code ec;
    fs::remove(archivePath, ec);
    DiagnosticReporter wrep;
    ASSERT_TRUE(linkAndWriteStaticArchive(
        std::span<AssembledModule const>{&libMod, 1},
        std::span<std::string const>{&memberName, 1},
        **target, **fmt, archivePath, wrep))
        << "errs=" << wrep.errorCount();

    // The client that REFERENCES the name — on its own it must pull the
    // member. That is the CONTROL: without it, "nothing was pulled" below
    // would be equally consistent with "this archive index is empty" or "the
    // pull never runs", which would say nothing at all.
    AssembledModule caller;
    caller.expectedFuncCount = 1;
    AssembledFunction callerFn;
    callerFn.symbol = SymbolId{41};
    callerFn.bytes  = {0xC3};
    caller.functions.push_back(std::move(callerFn));
    caller.externImports.push_back(ExternImport{SymbolId{40}, "pull_hidden", ""});

    std::vector<fs::path> const archives{archivePath};
    {
        DiagnosticReporter rep;
        auto const pulled = pullStaticArchiveMembers(
            std::span<AssembledModule const>{&caller, 1}, archives, {},
            **target, **fmt, rep);
        ASSERT_TRUE(pulled.has_value()) << "errs=" << rep.errorCount();
        EXPECT_EQ(pulled->size(), 1u)
            << "CONTROL: an unsatisfied reference to a HIDDEN definition must "
               "pull the member that defines it — the widened armap is what "
               "makes that resolvable, and a foreign `ld` does the same";
    }

    // The SUBJECT: a second module already in the link DEFINES the name, with
    // hidden visibility. Nothing may be pulled — the reference is satisfied.
    {
        AssembledModule provider;
        provider.expectedFuncCount = 1;
        AssembledFunction provFn;
        provFn.symbol = SymbolId{42};
        provFn.bytes  = {0xC3};
        provider.functions.push_back(std::move(provFn));
        provider.symbols.push_back(ModuleSymbol{SymbolId{42}, "pull_hidden",
                                               SymbolBinding::Global,
                                               SymbolVisibility::Hidden});
        std::vector<AssembledModule> clients;
        clients.push_back(caller);
        clients.push_back(std::move(provider));

        DiagnosticReporter rep;
        auto const pulled = pullStaticArchiveMembers(clients, archives, {},
                                                     **target, **fmt, rep);
        ASSERT_TRUE(pulled.has_value()) << "errs=" << rep.errorCount();
        EXPECT_TRUE(pulled->empty())
            << "a HIDDEN definition already in the link SATISFIES the "
               "reference, so the archive member must not be pulled — pulling "
               "it merges two definitions of one symbol, and that hazard is "
               "created by the armap fix itself";
    }

    // ...and the boundary is still held at LOCAL: a `static` of the same name
    // satisfies nothing, because no other module may name it.
    {
        AssembledModule localProvider;
        localProvider.expectedFuncCount = 1;
        AssembledFunction lpFn;
        lpFn.symbol = SymbolId{43};
        lpFn.bytes  = {0xC3};
        localProvider.functions.push_back(std::move(lpFn));
        localProvider.symbols.push_back(
            ModuleSymbol{SymbolId{43}, "pull_hidden", SymbolBinding::Local,
                         SymbolVisibility::Default});
        std::vector<AssembledModule> clients;
        clients.push_back(caller);
        clients.push_back(std::move(localProvider));

        DiagnosticReporter rep;
        auto const pulled = pullStaticArchiveMembers(clients, archives, {},
                                                     **target, **fmt, rep);
        ASSERT_TRUE(pulled.has_value()) << "errs=" << rep.errorCount();
        EXPECT_EQ(pulled->size(), 1u)
            << "CONTROL: a `static` of the same name is internal linkage and "
               "satisfies NOTHING — the widened predicate stops at Local";
    }

    fs::remove(archivePath, ec);
}

// ── (3) THE REAL PIPELINE, on corpus sources that are themselves RUN
//        witnesses on the linux legs ──────────────────────────────────────────

TEST(ElfImageSymtabPartition, RealPipelineExecPublishesLocalBandForStatics) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    // ⚠ THE SOURCE IS READ FROM THE TREE, NEVER COPIED HERE, and it is the
    // example a SIBLING anchor already owns: `examples/c/macho_static_fn_image`
    // carries exactly this shape (two statics reached through a const
    // function-pointer table indexed by a runtime value) and its `expected.json`
    // ALREADY runs `elf64-x86_64-linux-exec` and `elf64-aarch64-linux-exec`
    // (plus pe64 and macho64), on the baseline AND the `release` arm. A second
    // corpus example with a byte-identical body would have added no runtime
    // discrimination while moving 13 dated figures in `examples/README.md`, so
    // this pin reads that one and the RUN witness is shared.
    fs::path const src = dss::test::repoRoot() / "examples" / "c"
                         / "macho_static_fn_image" / "main.c";
    ASSERT_TRUE(fs::exists(src)) << src.generic_string();

    ScratchDir scratch{Location::InsideRepo, "p61_elf_image_symtab_partition"};
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program            prog;
    DiagnosticReporter rep;
    prog.setOutputDir(outDir);
    int const rc = prog.compileFiles({src.generic_string()}, "c",
                                     {"x86_64:elf64-x86_64-linux-exec"}, rep);
    std::string diags;
    for (auto const& d : rep.all()) diags += "\n  " + d.actual;
    ASSERT_EQ(rc, 0) << "compile failed:" << diags;
    ASSERT_EQ(rep.errorCount(), 0u) << diags;

    auto const artifact = outDir / "main";
    ASSERT_TRUE(fs::exists(artifact)) << artifact.generic_string();
    auto const bytes = readFileBytes(artifact);
    ASSERT_FALSE(bytes.empty());

    auto const symtab = readSymbolTable(bytes, ".symtab", ".strtab");
    ASSERT_TRUE(symtab.found);
    // STN_UNDEF, the `.text` section symbol, the entry trampoline, the two
    // statics, then `global_helper` and `main`. The `exit` import lives in
    // `.dynsym`, and the image emits no data symbols
    // (D-LINK-ELF-IMAGE-NO-DATA-SYMBOLS-IN-SYMTAB), so the `table` object is
    // absent by design.
    ASSERT_EQ(symtab.records.size(), 7u)
        << "STN_UNDEF + `.text` + trampoline + static_helper + other_static "
           "+ global_helper + main";
    EXPECT_EQ(symtab.records[0].info, kInfoUndef);
    EXPECT_EQ(symtab.records[1].info, kInfoLocalSect);
    // The trampoline is functions[0] with a minted SymbolId and no declared
    // name, so it keeps the `sym_<id>` fallback; the id is minted per build and
    // is not asserted, its PREFIX and its band are.
    EXPECT_EQ(symtab.records[2].name.rfind("sym_", 0), 0u)
        << "the trampoline must keep the `sym_<id>` fallback: "
        << symtab.records[2].name;
    EXPECT_EQ(symtab.records[2].info, kInfoLocalFunc)
        << "the nameless trampoline is Local (no `ModuleSymbol` row)";
    EXPECT_EQ(symtab.records[3].name, "static_helper");
    EXPECT_EQ(symtab.records[3].info, kInfoLocalFunc)
        << "a `static` is STB_LOCAL — gcc 13.3.0 and clang 18.1.3 both agree";
    EXPECT_EQ(symtab.records[4].name, "other_static");
    EXPECT_EQ(symtab.records[4].info, kInfoLocalFunc);
    EXPECT_EQ(symtab.records[5].name, "global_helper");
    EXPECT_EQ(symtab.records[5].info, kInfoGlobalFunc)
        << "the CONTROL: an ordinary extern-linkage function stays STB_GLOBAL";
    EXPECT_EQ(symtab.records[6].name, "main");
    EXPECT_EQ(symtab.records[6].info, kInfoGlobalFunc);
    // Every one of them DEFAULT visibility: this source declares none, so a
    // st_other that moved would mean the visibility axis had leaked a value
    // from somewhere it was not stated.
    for (auto const& r : symtab.records)
        EXPECT_EQ(r.other, kVisDefault) << r.name;

    EXPECT_EQ(symtab.shInfo, 5u)
        << "`.symtab.sh_info` must name the first non-LOCAL symbol — 2 before "
           "this change, because nothing local ever followed the section row";
    EXPECT_EQ(elfSymtabFirstNonLocal(symtab.raw), 5u);
    EXPECT_EQ(elfSymtabPartitionBreach(symtab.raw, symtab.shInfo), "");

    auto const dynsym = readSymbolTable(bytes, ".dynsym", ".dynstr");
    ASSERT_TRUE(dynsym.found);
    EXPECT_EQ(elfSymtabFirstNonLocal(dynsym.raw), dynsym.shInfo);
    EXPECT_EQ(elfSymtabPartitionBreach(dynsym.raw, dynsym.shInfo), "");
    for (auto const& r : dynsym.records) {
        EXPECT_NE(r.name, "static_helper")
            << "a `static` must never reach the dynamic symbol table";
        EXPECT_NE(r.name, "other_static");
    }
}

// ── (4) THE VISIBILITY AXIS through the REAL pipeline, on the corpus example
//        whose hidden functions are CALLED ────────────────────────────────────
//
// D-LK-OBJECT-GLOBAL-HIDDEN-VISIBILITY-EMITTED-LOCAL. This is the example the
// anchor's own trigger prose said could not reach a relocatable writer: it
// claimed a Global+Hidden symbol is always either DCE-eliminated or routed to
// an exec writer that forces Global. `examples/c/gnu_attribute_linkage_positions_crosscu`
// refutes both halves — `vis_lead` is `visibility("hidden")` on a NON-static
// function that `main` CALLS, so nothing may drop it. Before this change DSS
// emitted it `LOCAL sym_95` in the `.o` and `LOCAL vis_lead` in the image,
// where gcc 13.3.0 and clang 18.1.3 both emit `GLOBAL HIDDEN vis_lead` in BOTH.
//
// ⚠ HOW THE `.o` TIER IS REACHED, stated exactly, because an earlier account
// of this pin said the example's own manifest routed it there and THAT WAS
// FALSE. `expected.json` declares FOUR targets and every one of them is an
// IMAGE (`pe64-x86_64-windows-exec`, `elf64-x86_64-linux-exec`,
// `elf64-aarch64-linux-exec`, `macho64-arm64-darwin-exec`); it declares no
// `.o` and no `-staticlib` target at all. The relocatable tier is reached
// (a) FROM THE CLI — `dsscp --target x86_64:elf64-x86_64-linux` (or any
// `-staticlib` spec) over this or any source with a hidden non-static
// definition, which is a shipped, user-reachable invocation — and (b) HERE, by
// this pin, which drives `Program::compileUnits` over the SAME two sources
// with the ET_REL spec as its second tier. So the corpus manifest is the RUN
// witness for the IMAGE half only, and the `.o` half's witness is this test.
//
// ★★ `vis_tail` IS NOW A SECOND SUBJECT, AND THE FLIP IS THE POINT.
//
// This pin used to assert `vis_tail` is `GLOBAL DEFAULT` and say so as a
// CONTROL, because its `visibility("hidden")` sits on a PROTOTYPE and the
// definition follows, and the front end did not merge a declaration's declared
// linkage onto the definition — so its `ModuleSymbol` carried `Default` while
// gcc 13.3.0 and clang 18.1.3 both emit `GLOBAL HIDDEN`. That control carried
// its own expiry in place: "If this ever reads HIDDEN the gap closed and this
// control must become a second subject."
//
// The gap closed — D-C-DECLARED-LINKAGE-FACET-NOT-MERGED-ACROSS-A-REDECLARATION,
// fixed in the HIR linkage fold (`cst_to_hir.cpp`'s `mergeDeclaredLinkage`),
// NOT in any writer — so the assertion is INVERTED rather than deleted: it now
// demands `STV_HIDDEN`, which is strictly STRONGER than what it demanded before
// and which the whole writer path below must carry unchanged. `vis_lead` (the
// definition-position spelling) stays the first subject, and `main` remains the
// GLOBAL DEFAULT control that makes either reading possible at all.
//
// ⚠ THE TWO SUBJECTS ARE NOT REDUNDANT. They enter the writer from DIFFERENT
// front-end paths — a definition's own fold versus a fold inherited across a
// redeclaration — and only `vis_tail` can catch the inheritance being lost
// between HIR and the emitted `st_other`.
TEST(ElfHiddenVisibility, CalledHiddenFunctionKeepsGlobalBindingOnBothTiers) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    fs::path const dir = dss::test::repoRoot() / "examples" / "c"
                         / "gnu_attribute_linkage_positions_crosscu";
    fs::path const cuA = dir / "cu_a.c";
    fs::path const cuB = dir / "cu_b.c";
    ASSERT_TRUE(fs::exists(cuA)) << cuA.generic_string();
    ASSERT_TRUE(fs::exists(cuB)) << cuB.generic_string();

    struct Tier {
        char const* spec;
        char const* artifact;
        char const* label;
    };
    // The IMAGE tier and the RELOCATABLE tier of one source: the anchor's two
    // halves, and the tiers that answer two different questions about the same
    // symbol.
    std::vector<Tier> const tiers{
        {"x86_64:elf64-x86_64-linux-exec", "cu_a", "elf exec image"},
        {"x86_64:elf64-x86_64-linux", "cu_a.o", "elf ET_REL object"},
    };

    for (Tier const& tier : tiers) {
        ScratchDir scratch{Location::InsideRepo, "p61_elf_hidden_visibility"};
        scratch.useAsCwd();
        auto const outDir = scratch.path() / "out";

        Program            prog;
        DiagnosticReporter rep;
        prog.setOutputDir(outDir);
        // ⚠ `compileUnits`, NOT `compileFiles`: the example's `expected.json`
        // declares `sources`, which is the multi-CU model (one CompilationUnit
        // per file, merged at LINK). `compileFiles` folds them into ONE CU5
        // unit, where `gg`/`wp` — declared in cu_a.c and defined in cu_b.c —
        // must resolve WITHIN the unit and do not, so the pin would have been
        // measuring a different program from the one the corpus runs.
        int const rc = prog.compileUnits(
            {cuA.generic_string(), cuB.generic_string()}, "c", {tier.spec},
            rep);
        std::string diags;
        for (auto const& d : rep.all()) diags += "\n  " + d.actual;
        ASSERT_EQ(rc, 0) << tier.label << ": compile failed:" << diags;
        ASSERT_EQ(rep.errorCount(), 0u) << tier.label << diags;

        auto const artifact = outDir / tier.artifact;
        ASSERT_TRUE(fs::exists(artifact))
            << tier.label << ": " << artifact.generic_string();
        auto const bytes = readFileBytes(artifact);
        ASSERT_FALSE(bytes.empty()) << tier.label;

        auto const symtab = readSymbolTable(bytes, ".symtab", ".strtab");
        ASSERT_TRUE(symtab.found) << tier.label;

        // THE SUBJECT. Its real NAME must be present on BOTH tiers — before
        // this change the `.o` carved it to `sym_<id>` and there was no
        // `vis_lead` row at all.
        SymRecord const* const hidden = findRecord(symtab, "vis_lead");
        ASSERT_NE(hidden, nullptr)
            << tier.label
            << ": a `visibility(\"hidden\")` function has EXTERNAL LINKAGE and "
               "must keep its real name — a static link resolves it by name";
        EXPECT_EQ(hidden->info, kInfoGlobalFunc)
            << tier.label
            << ": ...with STB_GLOBAL, which is what gcc 13.3.0 and clang "
               "18.1.3 both emit; STB_LOCAL would say internal linkage";
        EXPECT_EQ(hidden->other, kVisHidden)
            << tier.label
            << ": ...and STV_HIDDEN in st_other, which is where ELF states "
               "the fact that binding cannot";

        // CONTROL 1 — an ordinary extern-linkage function of the SAME artifact
        // must stay GLOBAL DEFAULT. Without it, "vis_lead is GLOBAL" would be
        // equally consistent with "everything here is GLOBAL".
        SymRecord const* const control = findRecord(symtab, "main");
        ASSERT_NE(control, nullptr) << tier.label;
        EXPECT_EQ(control->info, kInfoGlobalFunc) << tier.label;
        EXPECT_EQ(control->other, kVisDefault)
            << tier.label
            << ": the CONTROL must NOT pick up the hidden row's st_other";

        // SUBJECT 2 — the PROTOTYPE-position spelling. Its `visibility("hidden")`
        // rides a declaration the definition does not repeat, so it reaches this
        // writer only if the front end folded the entity's declared linkage
        // across its declarations
        // (D-C-DECLARED-LINKAGE-FACET-NOT-MERGED-ACROSS-A-REDECLARATION).
        SymRecord const* const tail = findRecord(symtab, "vis_tail");
        ASSERT_NE(tail, nullptr) << tier.label;
        EXPECT_EQ(tail->info, kInfoGlobalFunc)
            << tier.label
            << ": a `visibility(\"hidden\")` function has EXTERNAL LINKAGE "
               "however the attribute reached it";
        EXPECT_EQ(tail->other, kVisHidden)
            << tier.label
            << ": `vis_tail`'s visibility rides a PROTOTYPE and the definition "
               "carries none of its own; DEFAULT here means the fold across the "
               "entity's declarations was lost and this symbol is EXPORTED where "
               "gcc 13.3.0 and clang 18.1.3 both emit GLOBAL HIDDEN";

        EXPECT_EQ(elfSymtabPartitionBreach(symtab.raw, symtab.shInfo), "")
            << tier.label;

        // The dynamic export set is the one place the hidden symbol must be
        // ABSENT — external linkage is not dynamic visibility.
        auto const dynsym = readSymbolTable(bytes, ".dynsym", ".dynstr");
        if (dynsym.found) {
            EXPECT_EQ(findRecord(dynsym, "vis_lead"), nullptr)
                << tier.label
                << ": a hidden symbol must never reach the dynamic export set";
            // …and the inherited-visibility spelling is held to the SAME rule.
            // Before the fold landed, `vis_tail` was DEFAULT and this assertion
            // would have been vacuous for the wrong reason.
            EXPECT_EQ(findRecord(dynsym, "vis_tail"), nullptr)
                << tier.label
                << ": a symbol made hidden by a PRIOR declaration is just as "
                   "hidden — the export set cannot depend on which declaration "
                   "spelled the attribute";
        }
    }
}
