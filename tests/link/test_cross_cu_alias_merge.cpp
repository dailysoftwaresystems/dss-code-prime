// D-LK-LINKER-MERGE-DROPS-EVERY-ALIAS-ROW — the cross-CU merge vs a definition
// that carries SEVERAL NAMES.
//
// Equal-offset defined symbols collapse to ONE atom under several names
// (D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS): `strong_fn` GLOBAL and
// `weak_alias` WEAK at one address is what gcc emits for
// `__attribute__((weak, alias("strong_fn")))`, what all three relocatable-object
// readers produce, and what `object_symbol_names.hpp`'s `definedAliases` reads. The
// LK11b merge (`linker.cpp::mergeModules`) answered every question about such a
// definition through its CANONICAL row alone:
//
//   * the `combined.symbols` rebuild deduped on the MERGED SYMBOL ID, so every row
//     after the first was dropped — a merged image lost every alias name from
//     `.symtab` / `.dynsym` / the export tables;
//   * the merged-id pre-assignment minted one id per NAME, so the second name of an
//     alias set got an id `remap` then refused, leaving `nameToId` pointing at an id
//     NO atom carries — the retarget of every relocation that named the alias from a
//     LOSING sibling definition, which the emission tier then rejects as undefined;
//   * the shadow test dropped BYTES on the canonical name's fate, so an atom that
//     lost its canonical name while WINNING an alias name had its body dropped and
//     the winning name left defined by nothing.
//
// Every pin below runs a REAL multi-module `linker::link` through a shipped format
// and reads the property back out of the EMITTED BYTES — the merge's own
// `combined.symbols` is not observable from outside (`mergeModules` is
// file-private), and the two halves are separable: a pin that only proved the row
// existed in the merged module would go green while the image still lost the name.
//
// The cross-CU questions an alias raises are pinned as behaviour, not as internals:
//   * a sibling CU REFERENCING the alias name binds to the aliased body (the extern
//     is stripped and the `call rel32` displacement lands on that body's VA);
//   * two CUs both defining the alias name STRONG is a redefinition and fails loud;
//   * one WEAK and one STRONG resolves, dropping the losing ROW while the losing
//     atom's OTHER names live on.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// ── Minimal ELF64 readers (local, like every other link-tier test) ──────────

[[nodiscard]] std::uint16_t readU16LE(std::vector<std::uint8_t> const& b,
                                      std::size_t off) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(b[off])
        | static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[off + 1]) << 8));
}
[[nodiscard]] std::uint32_t readU32LE(std::vector<std::uint8_t> const& b,
                                      std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[off + i]) << (i * 8);
    return v;
}
[[nodiscard]] std::uint64_t readU64LE(std::vector<std::uint8_t> const& b,
                                      std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    return v;
}

struct Shdr {
    std::string   name;
    std::uint64_t addr = 0, offset = 0, size = 0;
};

[[nodiscard]] std::string readCStr(std::vector<std::uint8_t> const& b,
                                   std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p) {
        s.push_back(static_cast<char>(b[p]));
    }
    return s;
}

[[nodiscard]] std::vector<Shdr> readSections(std::vector<std::uint8_t> const& b) {
    std::uint64_t const shoff    = readU64LE(b, 40);
    std::uint16_t const shnum    = readU16LE(b, 60);
    std::uint16_t const shstrndx = readU16LE(b, 62);
    std::uint64_t const strOff   = readU64LE(b, shoff + shstrndx * 64ull + 24);
    std::vector<Shdr> out;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const off = shoff + static_cast<std::uint64_t>(i) * 64;
        Shdr s;
        s.name   = readCStr(b, strOff + readU32LE(b, off + 0));
        s.addr   = readU64LE(b, off + 16);
        s.offset = readU64LE(b, off + 24);
        s.size   = readU64LE(b, off + 32);
        out.push_back(std::move(s));
    }
    return out;
}

[[nodiscard]] Shdr const* findSection(std::vector<Shdr> const& secs,
                                      std::string const& name) {
    for (auto const& s : secs) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// One Elf64_Sym: name u32 @0, info u8 @4, other u8 @5, shndx u16 @6,
// st_value u64 @8, st_size u64 @16 (24 bytes).
struct SymRow {
    std::string   name;
    std::uint8_t  bind = 0;   // st_info >> 4 (STB_LOCAL 0 / STB_GLOBAL 1 / STB_WEAK 2)
    std::uint64_t value = 0;
    std::uint64_t size  = 0;
};

// Every row of `symSection` (".symtab" or ".dynsym"), names resolved through
// `strSection`. Index 0 (the reserved null entry) is skipped — it has no name and
// would otherwise register as an empty-named row in every count.
[[nodiscard]] std::vector<SymRow>
readSymbols(std::vector<std::uint8_t> const& b, std::vector<Shdr> const& secs,
            std::string const& symSection, std::string const& strSection) {
    std::vector<SymRow> out;
    Shdr const* sy = findSection(secs, symSection);
    Shdr const* st = findSection(secs, strSection);
    if (sy == nullptr || st == nullptr) return out;
    for (std::uint64_t p = 24; p + 24 <= sy->size; p += 24) {
        std::uint64_t const off = sy->offset + p;
        out.push_back(SymRow{readCStr(b, st->offset + readU32LE(b, off)),
                             static_cast<std::uint8_t>(b[off + 4] >> 4),
                             readU64LE(b, off + 8),
                             readU64LE(b, off + 16)});
    }
    return out;
}

[[nodiscard]] std::vector<SymRow const*>
rowsNamed(std::vector<SymRow> const& rows, std::string const& name) {
    std::vector<SymRow const*> out;
    for (auto const& r : rows) {
        if (r.name == name) out.push_back(&r);
    }
    return out;
}

// The first byte offset of `needle` inside `hay`, or nullopt.
[[nodiscard]] std::optional<std::uint64_t>
findBytes(std::vector<std::uint8_t> const& hay, std::uint64_t begin, std::uint64_t end,
          std::vector<std::uint8_t> const& needle) {
    if (needle.empty() || end > hay.size() || begin + needle.size() > end) {
        return std::nullopt;
    }
    for (std::uint64_t i = begin; i + needle.size() <= end; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (hay[i + j] != needle[j]) { match = false; break; }
        }
        if (match) return i;
    }
    return std::nullopt;
}

// The VA a `call rel32` whose OPCODE byte sits at file offset `siteOff` branches to.
// disp is at siteOff+1; the next instruction starts at siteOff+5.
[[nodiscard]] std::uint64_t callTargetVa(std::vector<std::uint8_t> const& b,
                                         Shdr const& text, std::uint64_t siteOff) {
    auto const disp = static_cast<std::int32_t>(readU32LE(b, siteOff + 1));
    std::uint64_t const nextVa = text.addr + (siteOff + 5 - text.offset);
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(nextVa) + disp);
}

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShippedPair(std::string const& targetName,
                                     std::string const& formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped(targetName);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
        return out;
    }
    out.target = std::move(t).value();
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << formatName << ") failed";
        return out;
    }
    out.format = std::move(f).value();
    return out;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& rep,
                                    DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == code) ++n;
    }
    return n;
}

// ── Fixtures ────────────────────────────────────────────────────────────────

// The aliased body: `mov eax, 42; ret`. Unique in every image built here, so a byte
// search recovers its VA without consulting a single symbol name — the pins on WHERE
// a call lands must not be circular with the pins on WHICH names exist.
std::vector<std::uint8_t> const kAliasedBody{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
// A distinct rival body: `mov eax, 7; ret`.
std::vector<std::uint8_t> const kRivalBody{0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};

// CU #1: ONE function (`kAliasedBody`) carrying `names` — several `ModuleSymbol`
// rows on ONE `SymbolId`, which IS the alias representation.
[[nodiscard]] AssembledModule
makeAliasedDefiner(std::vector<ModuleSymbol> const& names) {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = kAliasedBody;
    m.functions.push_back(std::move(fn));
    m.symbols = names;
    return m;
}

// CU #2: ONE function `caller` that CALLS an extern named `externName` once.
[[nodiscard]] AssembledModule makeCaller(std::string externName, bool withEntry) {
    AssembledModule m;
    m.cuId = CompilationUnitId{2};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};   // call rel32 ; ret
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};          // rel32 (addendBias -4 lives in the schema)
    rel.addend = 0;
    fn.relocations.push_back(rel);
    m.functions.push_back(std::move(fn));
    ExternImport ext;
    ext.symbol      = SymbolId{2};
    ext.mangledName = std::move(externName);
    m.externImports.push_back(std::move(ext));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "caller", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    if (withEntry) m.userEntrySymbol = SymbolId{1};
    return m;
}

[[nodiscard]] ModuleSymbol sym(std::uint32_t id, std::string name,
                               SymbolBinding binding) {
    return ModuleSymbol{SymbolId{id}, std::move(name), binding,
                        SymbolVisibility::Default};
}

}  // namespace

// ── (1) The alias name reaches a FINAL IMAGE's `.symtab` ────────────────────
//
// Two CUs, an ET_EXEC link. CU #1 defines ONE body under `strong_fn` (GLOBAL) and
// `weak_alias` (WEAK); CU #2 calls `weak_alias`. The emitted `.symtab` must carry
// BOTH names, at the SAME st_value and st_size — one address, one extent, two names
// — with each row's own binding. Before the fix the merge dropped the second row, so
// the image's symtab named only `strong_fn` and `nm`/`gdb`/every profiler saw a body
// whose other ABI name did not exist.
TEST(CrossCuAliasMerge, AliasNameSurvivesTheMergeIntoTheImageSymtab) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    std::vector<AssembledModule> mods{
        makeAliasedDefiner({sym(1, "strong_fn", SymbolBinding::Global),
                            sym(1, "weak_alias", SymbolBinding::Weak)}),
        makeCaller("weak_alias", /*withEntry=*/true)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);
    for (auto const& d : rep.all()) {
        EXPECT_NE(d.severity, DiagnosticSeverity::Error) << diagnosticCodeName(d.code) << ": " << d.actual;
    }
    ASSERT_TRUE(img.ok());

    auto const secs = readSections(img.bytes);
    auto const syms = readSymbols(img.bytes, secs, ".symtab", ".strtab");

    auto const strongRows = rowsNamed(syms, "strong_fn");
    auto const aliasRows  = rowsNamed(syms, "weak_alias");
    ASSERT_EQ(strongRows.size(), 1u) << "the canonical name must appear exactly once";
    ASSERT_EQ(aliasRows.size(), 1u)
        << "the ALIAS name is absent from the merged image — the merge dropped every "
           "ModuleSymbol row after the first for one SymbolId "
           "(D-LK-LINKER-MERGE-DROPS-EVERY-ALIAS-ROW)";
    // One atom: identical address and extent, different names, each its own binding.
    EXPECT_EQ(aliasRows[0]->value, strongRows[0]->value);
    EXPECT_EQ(aliasRows[0]->size, strongRows[0]->size);
    EXPECT_EQ(aliasRows[0]->size, kAliasedBody.size());
    EXPECT_EQ(strongRows[0]->bind, 1u);   // STB_GLOBAL
    EXPECT_EQ(aliasRows[0]->bind, 2u);    // STB_WEAK

    // And the address is the aliased body's, located by BYTES rather than by name.
    Shdr const* text = findSection(secs, ".text");
    ASSERT_NE(text, nullptr);
    auto const bodyOff = findBytes(img.bytes, text->offset, text->offset + text->size,
                                   kAliasedBody);
    ASSERT_TRUE(bodyOff.has_value());
    EXPECT_EQ(strongRows[0]->value, text->addr + (*bodyOff - text->offset));
}

// ── (2) A sibling CU's REFERENCE to the alias name binds to the aliased body ─
//
// The reference half of the same link. `resolveCrossCuSymbols` matches by NAME, and
// an alias name is a real externally-visible name, so CU #2's extern `weak_alias`
// must resolve to CU #1's definition: the import is STRIPPED (it is not a library
// import) and the `call rel32` displacement resolves EXACTLY to the aliased body's
// VA. Asserting the DISPLACEMENT, not merely a clean link, is what distinguishes a
// correct bind from a branch into the wrong bytes.
TEST(CrossCuAliasMerge, ReferenceToTheAliasNameBindsToTheAliasedBody) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    std::vector<AssembledModule> mods{
        makeAliasedDefiner({sym(1, "strong_fn", SymbolBinding::Global),
                            sym(1, "weak_alias", SymbolBinding::Weak)}),
        makeCaller("weak_alias", /*withEntry=*/true)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);
    ASSERT_TRUE(img.ok());

    // The alias name resolved to a sibling definition, so it is NOT an import.
    EXPECT_EQ(std::count(img.externImportNames.begin(), img.externImportNames.end(),
                         std::string{"weak_alias"}),
              0);
    // The resolver saw the alias as its own externally-visible definition name.
    EXPECT_TRUE(img.resolvedGlobalDefs.contains("weak_alias"));
    EXPECT_TRUE(img.resolvedGlobalDefs.contains("strong_fn"));
    EXPECT_EQ(img.resolvedGlobalDefs.at("weak_alias"),
              img.resolvedGlobalDefs.at("strong_fn"))
        << "both names denote ONE definition, so both must resolve to one key";

    auto const secs = readSections(img.bytes);
    Shdr const* text = findSection(secs, ".text");
    ASSERT_NE(text, nullptr);
    auto const bodyOff = findBytes(img.bytes, text->offset, text->offset + text->size,
                                   kAliasedBody);
    ASSERT_TRUE(bodyOff.has_value());
    std::uint64_t const bodyVa = text->addr + (*bodyOff - text->offset);

    std::vector<std::uint8_t> const callerBody{0xE8, 0, 0, 0, 0, 0xC3};
    // Locate the call site by its `ret`-terminated shape rather than by its zeroed
    // displacement — the displacement is exactly what the link fills in.
    std::optional<std::uint64_t> site;
    for (std::uint64_t p = text->offset; p + 6 <= text->offset + text->size; ++p) {
        if (img.bytes[p] == 0xE8 && img.bytes[p + 5] == 0xC3
            && callTargetVa(img.bytes, *text, p) == bodyVa) {
            site = p;
            break;
        }
    }
    ASSERT_TRUE(site.has_value())
        << "no `call rel32` in .text resolves to the aliased body's VA — the "
           "cross-CU reference to the ALIAS name did not bind to the body it aliases";
    EXPECT_EQ(callTargetVa(img.bytes, *text, *site), bodyVa);
    EXPECT_EQ(callerBody.size(), 6u);
}

// ── (3) The alias name reaches an ET_DYN `.dynsym` export table ─────────────
//
// The `.so` arm, and it is a DIFFERENT emission path from (1): the ET_DYN export set
// walks `module.symbols` DIRECTLY (one `.dynsym` row per externally-visible named
// row) while the `.symtab` pass of (1) goes through `ObjectSymbolNames::
// definedAliases`. Pinning both is deliberate — the merge feeding the carrier and a
// writer reading it are separable, and a pin on one alone would go green while the
// other still lost the name. A library that exports `strong_fn` but not `weak_alias`
// fails `gcc main.c -L. -lfoo` at the alias name.
TEST(CrossCuAliasMerge, AliasNameIsExportedFromTheMergedDynamicLibrary) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-dyn");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    std::vector<AssembledModule> mods{
        makeAliasedDefiner({sym(1, "strong_fn", SymbolBinding::Global),
                            sym(1, "weak_alias", SymbolBinding::Weak)}),
        makeCaller("weak_alias", /*withEntry=*/false)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);
    for (auto const& d : rep.all()) {
        EXPECT_NE(d.severity, DiagnosticSeverity::Error) << diagnosticCodeName(d.code) << ": " << d.actual;
    }
    ASSERT_TRUE(img.ok());

    auto const secs = readSections(img.bytes);
    auto const dyn  = readSymbols(img.bytes, secs, ".dynsym", ".dynstr");

    auto const strongRows = rowsNamed(dyn, "strong_fn");
    auto const aliasRows  = rowsNamed(dyn, "weak_alias");
    auto const callerRows = rowsNamed(dyn, "caller");
    ASSERT_EQ(strongRows.size(), 1u);
    ASSERT_EQ(callerRows.size(), 1u) << "the merge's symbol rebuild is gone entirely";
    ASSERT_EQ(aliasRows.size(), 1u)
        << "the merged `.so` does not EXPORT the alias name "
           "(D-LK-LINKER-MERGE-DROPS-EVERY-ALIAS-ROW)";
    EXPECT_EQ(aliasRows[0]->value, strongRows[0]->value);
    EXPECT_EQ(aliasRows[0]->size, strongRows[0]->size);
    EXPECT_EQ(strongRows[0]->bind, 1u);   // STB_GLOBAL
    EXPECT_EQ(aliasRows[0]->bind, 2u);    // STB_WEAK
}

// ── (4) The alias name reaches a RE-EMITTED RELOCATABLE object's `.symtab` ──
//
// The ET_REL tier — the one D-LK-ALIAS-NAME-ABSENT-FROM-REEMITTED-OBJECT-SYMTAB is
// named for, and the tier where a lost name is a FOREIGN linker's "undefined
// reference". Two CUs merged into ONE `.o`.
TEST(CrossCuAliasMerge, AliasNameSurvivesIntoTheMergedRelocatableObject) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    std::vector<AssembledModule> mods{
        makeAliasedDefiner({sym(1, "strong_fn", SymbolBinding::Global),
                            sym(1, "weak_alias", SymbolBinding::Weak)}),
        makeCaller("weak_alias", /*withEntry=*/false)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);
    for (auto const& d : rep.all()) {
        EXPECT_NE(d.severity, DiagnosticSeverity::Error) << diagnosticCodeName(d.code) << ": " << d.actual;
    }
    ASSERT_TRUE(img.ok());

    auto const secs = readSections(img.bytes);
    auto const syms = readSymbols(img.bytes, secs, ".symtab", ".strtab");
    ASSERT_EQ(rowsNamed(syms, "strong_fn").size(), 1u);
    ASSERT_EQ(rowsNamed(syms, "weak_alias").size(), 1u)
        << "the merged relocatable object lost the alias name — a foreign linker "
           "resolving it reports `undefined reference to weak_alias`";
    EXPECT_EQ(rowsNamed(syms, "weak_alias")[0]->value,
              rowsNamed(syms, "strong_fn")[0]->value);
}

// ── (5) A REPEATED IDENTICAL row is still collapsed — the guard's real subject ─
//
// What the merged-id dedup PROTECTED, kept intact by the re-key. A reader may hand
// the merge two byte-identical rows for one id (`ObjectSymbolNames` classifies such
// a repeat as NOT an alias precisely because emitting it twice is a duplicate-symbol
// error at a foreign linker). The dedup key moved from `mergedId` to
// `(mergedId, name)`, which still collapses this and no longer collapses an alias —
// so this pin, not a comment, is the evidence the guard was re-keyed rather than
// weakened.
//
// ★ AND IT FOUND A SECOND DEFECT, one row upstream. ✔MEASURED by running this pin
// against the merged-symtab fix alone: the link did not merely lose a name, it was
// REFUSED — `resolveCrossCuSymbols` fed the repeated row to the resolution kernel
// TWICE, which ranked the definition's key against ITSELF and reported
// `K_SymbolRedefinedAcrossUnits: symbol "strong_fn" has multiple strong (Global)
// definitions across CompilationUnits (CU #1 and CU #1)`. One CU named as both
// parties is the tell: there is no second definition to find. The flatten now emits
// one triple per DEFINITION rather than per ROW, so the STRONG repeat behaves like
// the Weak and Local repeats always did. This test therefore asserts the clean link
// (`img.ok()`, no Error diagnostic) as well as the collapse.
TEST(CrossCuAliasMerge, RepeatedIdenticalSymbolRowStillCollapsesToOne) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    std::vector<AssembledModule> mods{
        makeAliasedDefiner({sym(1, "strong_fn", SymbolBinding::Global),
                            sym(1, "strong_fn", SymbolBinding::Global),
                            sym(1, "weak_alias", SymbolBinding::Weak)}),
        makeCaller("strong_fn", /*withEntry=*/true)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);
    for (auto const& d : rep.all()) {
        EXPECT_NE(d.severity, DiagnosticSeverity::Error) << diagnosticCodeName(d.code) << ": " << d.actual;
    }
    ASSERT_TRUE(img.ok());

    auto const secs = readSections(img.bytes);
    auto const syms = readSymbols(img.bytes, secs, ".symtab", ".strtab");
    EXPECT_EQ(rowsNamed(syms, "strong_fn").size(), 1u)
        << "a repeated IDENTICAL row must still collapse — the dedup was re-keyed, "
           "not removed";
    EXPECT_EQ(rowsNamed(syms, "weak_alias").size(), 1u);
}

// ── (6) Two CUs defining the SAME alias name STRONG is a redefinition ───────
//
// An alias name is a definition like any other, so it must NOT get a quieter
// resolution than the canonical name would. Fail loud, and emit nothing.
TEST(CrossCuAliasMerge, TwoStrongDefinitionsOfOneAliasNameFailLoud) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    AssembledModule rival;
    rival.cuId = CompilationUnitId{2};
    rival.expectedFuncCount = 1;
    AssembledFunction rivalFn;
    rivalFn.symbol = SymbolId{1};
    rivalFn.bytes  = kRivalBody;
    rival.functions.push_back(std::move(rivalFn));
    rival.symbols.push_back(sym(1, "weak_alias", SymbolBinding::Global));
    rival.userEntrySymbol = SymbolId{1};

    std::vector<AssembledModule> mods{
        makeAliasedDefiner({sym(1, "strong_fn", SymbolBinding::Global),
                            sym(1, "weak_alias", SymbolBinding::Global)}),
        std::move(rival)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 1u)
        << "a strong redefinition reached through an ALIAS name must fail loud "
           "exactly as one reached through a canonical name does";
    EXPECT_FALSE(img.ok());
    EXPECT_TRUE(img.bytes.empty());
}

// ── (7) A LOSING alias row is dropped; the losing atom's other names live on ─
//
// One WEAK `weak_alias` (on CU #1's aliased body) against a sibling CU's STRONG
// `weak_alias`. The strong one wins, so:
//   * `weak_alias` appears EXACTLY ONCE in the image, at the STRONG definition's
//     body — never twice, which would put one ABI name on two addresses;
//   * CU #1's body still lives under `strong_fn`, which nothing contested. Dropping
//     it because its ALIAS lost would delete a definition the link still needs.
TEST(CrossCuAliasMerge, LosingAliasRowIsDroppedWhileItsAtomKeepsItsOtherName) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    AssembledModule rival;
    rival.cuId = CompilationUnitId{2};
    rival.expectedFuncCount = 1;
    AssembledFunction rivalFn;
    rivalFn.symbol = SymbolId{1};
    rivalFn.bytes  = kRivalBody;
    rival.functions.push_back(std::move(rivalFn));
    rival.symbols.push_back(sym(1, "weak_alias", SymbolBinding::Global));
    rival.userEntrySymbol = SymbolId{1};

    std::vector<AssembledModule> mods{
        makeAliasedDefiner({sym(1, "strong_fn", SymbolBinding::Global),
                            sym(1, "weak_alias", SymbolBinding::Weak)}),
        std::move(rival)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);
    for (auto const& d : rep.all()) {
        EXPECT_NE(d.severity, DiagnosticSeverity::Error) << diagnosticCodeName(d.code) << ": " << d.actual;
    }
    ASSERT_TRUE(img.ok());

    auto const secs = readSections(img.bytes);
    Shdr const* text = findSection(secs, ".text");
    ASSERT_NE(text, nullptr);
    auto const aliasedOff = findBytes(img.bytes, text->offset,
                                      text->offset + text->size, kAliasedBody);
    auto const rivalOff   = findBytes(img.bytes, text->offset,
                                      text->offset + text->size, kRivalBody);
    ASSERT_TRUE(aliasedOff.has_value())
        << "CU #1's body was dropped although the name it lost was only its ALIAS — "
           "`strong_fn` is now defined by nothing";
    ASSERT_TRUE(rivalOff.has_value());

    auto const syms = readSymbols(img.bytes, secs, ".symtab", ".strtab");
    auto const aliasRows = rowsNamed(syms, "weak_alias");
    ASSERT_EQ(aliasRows.size(), 1u)
        << "the losing WEAK alias row must be dropped — one ABI name cannot name two "
           "addresses in one image";
    EXPECT_EQ(aliasRows[0]->value, text->addr + (*rivalOff - text->offset))
        << "the surviving `weak_alias` must be the STRONG definition's";
    auto const strongRows = rowsNamed(syms, "strong_fn");
    ASSERT_EQ(strongRows.size(), 1u);
    EXPECT_EQ(strongRows[0]->value, text->addr + (*aliasedOff - text->offset));
}

// ── (8) A losing definition of an aliased name retargets to the WINNER ──────
//
// The merged-id half of the defect, and the reason the pre-assignment mints per KEY
// rather than per NAME. CU #1's ONE body carries TWO winning names; CU #2 defines a
// WEAK loser for EACH of them and calls both. Every one of CU #2's references must
// retarget onto CU #1's body.
//
// ★ BOTH names are exercised deliberately. The per-name mint burned an id for
// whichever name `resolvedGlobalDefs` (an unordered_map) happened to iterate SECOND,
// so exactly one of the two was left folding onto an id no atom carries — which one
// is not knowable from the source. Exercising a single name would have made this pin
// a coin flip; exercising both makes it deterministic.
TEST(CrossCuAliasMerge, LosingDefinitionOfAnAliasedNameRetargetsToTheWinningBody) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    AssembledModule loser;
    loser.cuId = CompilationUnitId{2};
    loser.expectedFuncCount = 3;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    caller.bytes  = {0xE8, 0, 0, 0, 0,      // call rel32  -> weak `alias_a`
                     0xE8, 0, 0, 0, 0,      // call rel32  -> weak `alias_b`
                     0xC3};                 // ret
    for (auto const& [off, target] :
         std::vector<std::pair<std::uint64_t, std::uint32_t>>{{1, 2}, {6, 3}}) {
        Relocation rel;
        rel.offset = off;
        rel.target = SymbolId{target};
        rel.kind   = RelocationKind{1};
        rel.addend = 0;
        caller.relocations.push_back(rel);
    }
    loser.functions.push_back(std::move(caller));
    for (std::uint32_t id : {2u, 3u}) {
        AssembledFunction weakFn;
        weakFn.symbol = SymbolId{id};
        weakFn.bytes  = {0xB8, static_cast<std::uint8_t>(0x50 + id),
                         0x00, 0x00, 0x00, 0xC3};
        loser.functions.push_back(std::move(weakFn));
    }
    loser.symbols.push_back(sym(1, "caller", SymbolBinding::Global));
    loser.symbols.push_back(sym(2, "alias_a", SymbolBinding::Weak));
    loser.symbols.push_back(sym(3, "alias_b", SymbolBinding::Weak));
    loser.userEntrySymbol = SymbolId{1};

    std::vector<AssembledModule> mods{
        makeAliasedDefiner({sym(1, "alias_a", SymbolBinding::Global),
                            sym(1, "alias_b", SymbolBinding::Global)}),
        std::move(loser)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);
    for (auto const& d : rep.all()) {
        EXPECT_NE(d.severity, DiagnosticSeverity::Error) << diagnosticCodeName(d.code) << ": " << d.actual;
    }
    ASSERT_TRUE(img.ok())
        << "a relocation naming one of the alias set's names retargeted onto a "
           "merged id no definition carries (the per-NAME id mint burned it)";

    auto const secs = readSections(img.bytes);
    Shdr const* text = findSection(secs, ".text");
    ASSERT_NE(text, nullptr);
    auto const bodyOff = findBytes(img.bytes, text->offset, text->offset + text->size,
                                   kAliasedBody);
    ASSERT_TRUE(bodyOff.has_value());
    std::uint64_t const bodyVa = text->addr + (*bodyOff - text->offset);

    // Both call sites sit in the one surviving caller: `E8 .. E8 .. C3`.
    std::optional<std::uint64_t> callerOff;
    for (std::uint64_t p = text->offset; p + 11 <= text->offset + text->size; ++p) {
        if (img.bytes[p] == 0xE8 && img.bytes[p + 5] == 0xE8
            && img.bytes[p + 10] == 0xC3) {
            callerOff = p;
            break;
        }
    }
    ASSERT_TRUE(callerOff.has_value());
    EXPECT_EQ(callTargetVa(img.bytes, *text, *callerOff), bodyVa)
        << "the reference through `alias_a` did not land on the winning body";
    EXPECT_EQ(callTargetVa(img.bytes, *text, *callerOff + 5), bodyVa)
        << "the reference through `alias_b` did not land on the winning body";

    // Both losing bodies are gone: the winner's is the only `mov eax, imm; ret` of
    // its shape left, and each loser's distinctive immediate is absent.
    for (std::uint32_t id : {2u, 3u}) {
        std::vector<std::uint8_t> const loserBody{
            0xB8, static_cast<std::uint8_t>(0x50 + id), 0x00, 0x00, 0x00, 0xC3};
        EXPECT_FALSE(findBytes(img.bytes, text->offset, text->offset + text->size,
                               loserBody)
                         .has_value())
            << "a shadowed body survived into the merged image";
    }
}

// ── (9) A DROPPED atom's compiler-private label must not migrate onto the winner ─
//
// The other half of "the bytes and the names are two questions". A defined atom can
// carry a NON-externally-visible extra row — `object_symbol_names.hpp` records the
// MEASURED case: clang emits `ltmp0` at the exact `n_value` of the first function of
// every ordinary translation unit, and all three readers fold it in as an extra name
// on the atom. Such a row cannot lose a cross-CU resolution (Local names are
// module-private and never enter the winner table), so per-NAME filtering alone lets
// it through even when its ATOM was folded away — and it is then re-keyed onto the
// WINNER's merged id.
//
// That is not a cosmetic leak. The losing CU is walked FIRST here, so the migrated
// private row becomes the FIRST row for the winner's merged id, and
// `ObjectSymbolNames` is first-row-wins: the winning global's own name loses its
// canonical slot to a label from a body that is not even in the image, and on the
// relocatable tier `definedBinding` then carves it to STB_LOCAL `sym_<id>`.
TEST(CrossCuAliasMerge, DroppedAtomsPrivateLabelDoesNotMigrateOntoTheWinner) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    // CU #1 (walked first): the LOSING atom — a WEAK `contested` plus the
    // compiler-private `ltmp0` label that shares its address.
    AssembledModule loser =
        makeAliasedDefiner({sym(1, "contested", SymbolBinding::Weak),
                            sym(1, "ltmp0", SymbolBinding::Local)});

    // CU #2: the STRONG winner of `contested`, and the entry.
    AssembledModule winner;
    winner.cuId = CompilationUnitId{2};
    winner.expectedFuncCount = 1;
    AssembledFunction winnerFn;
    winnerFn.symbol = SymbolId{1};
    winnerFn.bytes  = kRivalBody;
    winner.functions.push_back(std::move(winnerFn));
    winner.symbols.push_back(sym(1, "contested", SymbolBinding::Global));
    winner.userEntrySymbol = SymbolId{1};

    std::vector<AssembledModule> mods{std::move(loser), std::move(winner)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);
    for (auto const& d : rep.all()) {
        EXPECT_NE(d.severity, DiagnosticSeverity::Error)
            << diagnosticCodeName(d.code) << ": " << d.actual;
    }
    ASSERT_TRUE(img.ok());

    auto const secs = readSections(img.bytes);
    Shdr const* text = findSection(secs, ".text");
    ASSERT_NE(text, nullptr);
    auto const winnerOff = findBytes(img.bytes, text->offset,
                                     text->offset + text->size, kRivalBody);
    ASSERT_TRUE(winnerOff.has_value());
    std::uint64_t const winnerVa = text->addr + (*winnerOff - text->offset);

    auto const syms = readSymbols(img.bytes, secs, ".symtab", ".strtab");
    EXPECT_TRUE(rowsNamed(syms, "ltmp0").empty())
        << "the dropped atom's private label reached the image — and because its CU "
           "is walked first it is now the CANONICAL row of the WINNING definition";
    auto const contestedRows = rowsNamed(syms, "contested");
    ASSERT_EQ(contestedRows.size(), 1u);
    EXPECT_EQ(contestedRows[0]->value, winnerVa);
    // The losing body itself is gone, as it always was.
    EXPECT_FALSE(findBytes(img.bytes, text->offset, text->offset + text->size,
                           kAliasedBody)
                     .has_value());
}

// ── (10) An atom that loses its CANONICAL name but WINS an alias name lives ──
//
// The body half of the same split, and the arm that is invisible if the shadow
// question is asked of the canonical row alone. CU #1's one body is `contested`
// (WEAK, canonical) and `kept_name` (GLOBAL); a sibling CU defines `contested`
// STRONG. Only the FIRST name lost. Deciding the BYTES from the canonical row
// therefore deletes a body that is still the sole definition of `kept_name`, and
// nothing downstream can notice: `kept_name` simply stops existing, and a reference
// to it from anywhere else in the link becomes an undefined symbol whose cause is
// three stages upstream.
//
// The row ORDER here is the whole point — `contested` is listed first, so it is the
// canonical row. Reversing the two names would make this pass against either rule.
TEST(CrossCuAliasMerge, AtomThatLosesItsCanonicalNameButWinsAnAliasNameSurvives) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    AssembledModule winner;
    winner.cuId = CompilationUnitId{2};
    winner.expectedFuncCount = 1;
    AssembledFunction winnerFn;
    winnerFn.symbol = SymbolId{1};
    winnerFn.bytes  = kRivalBody;
    winner.functions.push_back(std::move(winnerFn));
    winner.symbols.push_back(sym(1, "contested", SymbolBinding::Global));
    winner.userEntrySymbol = SymbolId{1};

    std::vector<AssembledModule> mods{
        makeAliasedDefiner({sym(1, "contested", SymbolBinding::Weak),
                            sym(1, "kept_name", SymbolBinding::Global)}),
        std::move(winner)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);
    for (auto const& d : rep.all()) {
        EXPECT_NE(d.severity, DiagnosticSeverity::Error)
            << diagnosticCodeName(d.code) << ": " << d.actual;
    }
    ASSERT_TRUE(img.ok());

    auto const secs = readSections(img.bytes);
    Shdr const* text = findSection(secs, ".text");
    ASSERT_NE(text, nullptr);
    auto const keptOff = findBytes(img.bytes, text->offset, text->offset + text->size,
                                   kAliasedBody);
    ASSERT_TRUE(keptOff.has_value())
        << "the body was dropped because its CANONICAL name lost — but it WON "
           "`kept_name`, which is now defined by nothing";
    auto const winnerOff = findBytes(img.bytes, text->offset,
                                     text->offset + text->size, kRivalBody);
    ASSERT_TRUE(winnerOff.has_value());

    auto const syms = readSymbols(img.bytes, secs, ".symtab", ".strtab");
    auto const keptRows = rowsNamed(syms, "kept_name");
    ASSERT_EQ(keptRows.size(), 1u);
    EXPECT_EQ(keptRows[0]->value, text->addr + (*keptOff - text->offset));
    auto const contestedRows = rowsNamed(syms, "contested");
    ASSERT_EQ(contestedRows.size(), 1u)
        << "the lost canonical name must be emitted once, by the CU that won it";
    EXPECT_EQ(contestedRows[0]->value, text->addr + (*winnerOff - text->offset));
}

// ── (11) A folded atom whose FIRST row is a private label is still folded ────
//
// The row-ORDER arm, and the one the canonical-row shortcut gets wrong. ELF orders
// `.symtab` LOCALS BEFORE GLOBALS, so an object re-read from disk hands the merge a
// compiler-private label FIRST and the real ABI name second — `ltmp0` (Local) then
// `contested` (Weak) on one atom is exactly what a reader produces from that
// ordering.
//
// Asking only the CANONICAL row whether the atom folds answers "it is Local, so it
// is module-private, so keep it" — while `mergedIdFor`, which skips Local rows
// looking for a name to fold on, hands that same atom the WINNER's merged id. The
// two decisions then disagree: the body is emitted carrying an id another body
// already carries, and the merged module fails its own within-image duplicate-
// SymbolId gate. A private label in the first row must not exempt an atom from
// being replaced by the definition that won its real name.
//
// ✔MEASURED: without this pin the canonical-row shortcut SURVIVED the whole
// red-on-disable sweep — every other cell here is satisfied by the `winningKeys`
// short-circuit, which the shortcut keeps.
TEST(CrossCuAliasMerge, FoldedAtomWhosePrivateLabelIsListedFirstIsStillFolded) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    AssembledModule winner;
    winner.cuId = CompilationUnitId{2};
    winner.expectedFuncCount = 1;
    AssembledFunction winnerFn;
    winnerFn.symbol = SymbolId{1};
    winnerFn.bytes  = kRivalBody;
    winner.functions.push_back(std::move(winnerFn));
    winner.symbols.push_back(sym(1, "contested", SymbolBinding::Global));
    winner.userEntrySymbol = SymbolId{1};

    // The private label is listed FIRST — that is the whole fixture.
    std::vector<AssembledModule> mods{
        makeAliasedDefiner({sym(1, "ltmp0", SymbolBinding::Local),
                            sym(1, "contested", SymbolBinding::Weak)}),
        std::move(winner)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);
    for (auto const& d : rep.all()) {
        EXPECT_NE(d.severity, DiagnosticSeverity::Error)
            << diagnosticCodeName(d.code) << ": " << d.actual;
    }
    ASSERT_TRUE(img.ok())
        << "the losing atom was kept because its FIRST row is a private label, but "
           "its merged id was folded onto the winner — two bodies, one id";

    auto const secs = readSections(img.bytes);
    Shdr const* text = findSection(secs, ".text");
    ASSERT_NE(text, nullptr);
    EXPECT_FALSE(findBytes(img.bytes, text->offset, text->offset + text->size,
                           kAliasedBody)
                     .has_value())
        << "the replaced body must not be emitted";
    auto const syms = readSymbols(img.bytes, secs, ".symtab", ".strtab");
    EXPECT_TRUE(rowsNamed(syms, "ltmp0").empty());
    ASSERT_EQ(rowsNamed(syms, "contested").size(), 1u);
    auto const winnerOff = findBytes(img.bytes, text->offset,
                                     text->offset + text->size, kRivalBody);
    ASSERT_TRUE(winnerOff.has_value());
    EXPECT_EQ(rowsNamed(syms, "contested")[0]->value,
              text->addr + (*winnerOff - text->offset));
}

// ── (12) One definition whose names win in TWO places is refused, not guessed ─
//
// The fail-loud arm, EXERCISED rather than read. A relocation names a DEFINITION,
// not the name it was written through, so when an atom carries several names and
// those names resolve to DIFFERENT definitions elsewhere, there is no correct
// retarget: either choice binds half of this CU's references to the wrong body, and
// both choices link clean. That is the "genuinely-unsupported merge interaction"
// K_CrossCuMergeUnsupported is reserved for.
//
// CU #1's single body is WEAK `shared_a` and WEAK `shared_b`; CU #2 defines
// `shared_a` STRONG and CU #3 defines `shared_b` STRONG, so CU #1's atom wins
// neither name and the two names fold onto two different atoms. CU #1 also calls its
// own atom — without that reference the atom is simply dropped and nothing has to be
// decided, which is why the caller is part of the fixture rather than decoration.
//
// Weak-vs-strong, not strong-vs-strong: two STRONG definitions of one name are a
// redefinition that `resolveCrossCuSymbols` refuses BEFORE the merge runs, so that
// shape can never reach this arm.
TEST(CrossCuAliasMerge, DefinitionWhoseNamesResolveToTwoDefinitionsIsRefused) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    // CU #1: the doubly-named body, plus an intra-CU caller that references it.
    AssembledModule doublyNamed =
        makeAliasedDefiner({sym(1, "shared_a", SymbolBinding::Weak),
                            sym(1, "shared_b", SymbolBinding::Weak)});
    doublyNamed.expectedFuncCount = 2;
    AssembledFunction localCaller;
    localCaller.symbol = SymbolId{2};
    localCaller.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};   // call rel32 -> SymbolId{1}
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{1};
    rel.addend = 0;
    localCaller.relocations.push_back(rel);
    doublyNamed.functions.push_back(std::move(localCaller));
    doublyNamed.symbols.push_back(sym(2, "cu1_caller", SymbolBinding::Global));

    auto strongDefiner = [](std::uint32_t cu, std::string name, std::uint8_t imm,
                            bool entry) {
        AssembledModule m;
        m.cuId = CompilationUnitId{cu};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = {0xB8, imm, 0x00, 0x00, 0x00, 0xC3};
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, std::move(name),
                                         SymbolBinding::Global,
                                         SymbolVisibility::Default});
        if (entry) m.userEntrySymbol = SymbolId{1};
        return m;
    };

    std::vector<AssembledModule> mods{
        std::move(doublyNamed),
        strongDefiner(2, "shared_a", 0x07, /*entry=*/true),
        strongDefiner(3, "shared_b", 0x09, /*entry=*/false)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);

    EXPECT_EQ(countCode(rep, DiagnosticCode::K_CrossCuMergeUnsupported), 1u)
        << "EXACTLY ONE refusal — the merge memoizes the fold, so a second report "
           "would mean the decision is being retaken per reference";
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 0u)
        << "weak-vs-strong is a resolution, not a redefinition — reporting that code "
           "here would send the reader hunting the wrong fault";
    EXPECT_FALSE(img.ok());
    EXPECT_TRUE(img.bytes.empty())
        << "no image may be emitted once the retarget is known to be undecidable";

    // The message must name the definition and BOTH of its names, or the author
    // cannot tell which alias set is at fault.
    bool sawIt = false;
    for (auto const& d : rep.all()) {
        if (d.code != DiagnosticCode::K_CrossCuMergeUnsupported) continue;
        sawIt = true;
        EXPECT_NE(d.actual.find("shared_a"), std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("shared_b"), std::string::npos) << d.actual;
        EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
    }
    EXPECT_TRUE(sawIt);
}

// ── (13) THREE names, THREE resolutions — still ONE diagnostic ───────────
//
// The cell above pins "exactly one refusal" at TWO names, which is the one width
// at which the two readings of "once" coincide: the memoizing `remap.emplace`
// makes the decision once per atom across CALLS, but the loop that detects the
// divergence used to `report` from inside itself, once per diverging NAME. At
// two names there is exactly one diverging name, so both readings say 1 and the
// pin could not tell them apart. At THREE it can: the pre-fix walker emitted TWO
// diagnostics from a single call, each pairing the canonical with one rival.
//
// The message must also carry the WHOLE set. A reader handed `"a" and "b"` and
// then `"a" and "c"` has to reassemble the alias set from two sentences to see
// what actually failed to agree — and would have no way to know a fourth name
// was not simply missing.
TEST(CrossCuAliasMerge, DefinitionWhoseThreeNamesResolveThreeWaysIsRefusedOnce) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    AssembledModule triplyNamed =
        makeAliasedDefiner({sym(1, "shared_a", SymbolBinding::Weak),
                            sym(1, "shared_b", SymbolBinding::Weak),
                            sym(1, "shared_c", SymbolBinding::Weak)});
    triplyNamed.expectedFuncCount = 2;
    AssembledFunction localCaller;   // the reference that forces the decision
    localCaller.symbol = SymbolId{2};
    localCaller.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};   // call rel32 -> SymbolId{1}
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{1};
    rel.addend = 0;
    localCaller.relocations.push_back(rel);
    triplyNamed.functions.push_back(std::move(localCaller));
    triplyNamed.symbols.push_back(sym(2, "cu1_caller", SymbolBinding::Global));

    auto strongDefiner = [](std::uint32_t cu, std::string name, std::uint8_t imm,
                            bool entry) {
        AssembledModule m;
        m.cuId = CompilationUnitId{cu};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = {0xB8, imm, 0x00, 0x00, 0x00, 0xC3};
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, std::move(name),
                                         SymbolBinding::Global,
                                         SymbolVisibility::Default});
        if (entry) m.userEntrySymbol = SymbolId{1};
        return m;
    };

    std::vector<AssembledModule> mods{
        std::move(triplyNamed),
        strongDefiner(2, "shared_a", 0x07, /*entry=*/true),
        strongDefiner(3, "shared_b", 0x09, /*entry=*/false),
        strongDefiner(4, "shared_c", 0x0B, /*entry=*/false)};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);

    EXPECT_EQ(countCode(rep, DiagnosticCode::K_CrossCuMergeUnsupported), 1u)
        << "ONE refusal for ONE undecidable atom, at any width of alias set — "
           "two here would mean the walker reports per diverging NAME and the "
           "author reads the same fault twice, paired differently";
    EXPECT_FALSE(img.ok());
    EXPECT_TRUE(img.bytes.empty());

    bool sawIt = false;
    for (auto const& d : rep.all()) {
        if (d.code != DiagnosticCode::K_CrossCuMergeUnsupported) continue;
        sawIt = true;
        EXPECT_NE(d.actual.find("shared_a"), std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("shared_b"), std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("shared_c"), std::string::npos)
            << "the single message must name EVERY name that failed to agree, "
               "or the set has to be reassembled from separate sentences: "
            << d.actual;
        EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
    }
    EXPECT_TRUE(sawIt);
}
