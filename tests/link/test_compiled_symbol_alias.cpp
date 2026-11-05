// THE COMPILED SHAPE OF A SYMBOL ALIAS, THROUGH THE LINK ON EVERY FORMAT
// (D-CSUBSET-ATTRIBUTE-ALIAS-TARGET-NO-SURFACE).
//
// `__attribute__((alias("g"))) int f(void)` gives the definition `g` a second
// external name. The link tier already models that as SEVERAL `ModuleSymbol` rows
// on ONE `SymbolId` (what all three relocatable-object readers make of a foreign
// object's alias, `ObjectSymbolNames::definedAliases`). What a COMPILED unit adds
// is how it refers to the alias: every use of `f` in the unit means the NAME — a
// weak alias is replaced by another unit's strong `f`, and the unit's own calls
// must follow it there — so a compiled unit states it the way an object reader
// states a weak name it keeps a reference on: a plain reference row (`ExternImport`)
// named `f`, and the alias row marked `referencedByName`.
//
// These cases hand-build exactly that module — a body `g`, a second row `f` on
// `g`'s id, a caller of `f` through a reference row — and read what the link makes
// of it on ELF, PE and Mach-O, as an image and as a relocatable object:
//   * the call lands on the aliased body, located by its BYTES;
//   * a relocatable object carries both names on the one body, and its reference
//     to `f` stays a reference to the NAME (read back by the format's own reader);
//   * a WEAK alias yields to another unit's strong definition of the name, and the
//     unit's own call follows the name there;
//   * a STRONG alias beside another unit's strong definition of the name is two
//     definitions, refused by name;
//   * a STRONG alias of a SELECT-ANY definition is a name of its body, read back as
//     its format states one.
// The module shape is the one `lowerMirModuleToAssembly` (src/program/
// compile_pipeline.cpp) emits for an alias record; these cases pin that the link
// tier takes it whole.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/image_request.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
};

[[nodiscard]] Loaded load(char const* target, char const* format) {
    Loaded l;
    auto t = TargetSchema::loadShipped(target);
    auto f = ObjectFormatSchema::loadShipped(format);
    EXPECT_TRUE(t.has_value() && f.has_value()) << target << " / " << format;
    if (t.has_value()) l.target = *t;
    if (f.has_value()) l.format = *f;
    return l;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

[[nodiscard]] bool reported(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return true;
    }
    return false;
}

// The aliased body: `mov eax, 42 ; ret`. A rival definition of the alias's name:
// `mov eax, 7 ; ret`. Each is unique in every image here, so a byte search finds
// its place without reading a single symbol name.
std::vector<std::uint8_t> const kAliasedBody{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
std::vector<std::uint8_t> const kRivalBody{0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};

// An image's own file name: a Mach-O image's code-signature identity is a
// function of it (`image.codeSignature.identifier`), and no other format reads it.
ImageRequest const kImage{.artifactFileName = "compiled_symbol_alias"};

// One shipped x86_64 format and the C spelling of a name on it (Mach-O decorates
// every C symbol with one leading underscore — its document's `cSymbolDecoration`),
// and what its document says becomes of a definition that lost an OVERRIDABLE weak
// name to another unit's definition in an image (`supersededDefinition`; each answer's
// measurement is stated in `link/object_format_schema.hpp`): ELF and PE keep its
// bytes under its other names, Mach-O replaces it whole, every label it carries
// naming the winner.
struct Format {
    char const*          image;
    char const*          object;
    char const*          prefix;
    SupersededDefinition overridden;
};
constexpr Format kFormats[] = {
    {"elf64-x86_64-linux-exec", "elf64-x86_64-linux", "", SupersededDefinition::KeepsItsBytes},
    {"pe64-x86_64-windows-exec", "pe64-x86_64-windows", "", SupersededDefinition::KeepsItsBytes},
    {"macho64-x86_64-darwin-exec", "macho64-x86_64-darwin", "_", SupersededDefinition::ReplacedWhole},
};

// THE COMPILED SHAPE: unit `cu` defines `g` (the aliased body) under `gBinding`,
// gives it the second name `f` under `fBinding`, and its entry `main` calls `f`
// through a plain reference row of the name.
[[nodiscard]] AssembledModule compiledAliasUnit(std::string const& prefix, SymbolBinding gBinding,
                                                SymbolBinding fBinding) {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    AssembledFunction g;
    g.symbol = SymbolId{1};
    g.bytes  = kAliasedBody;
    m.functions.push_back(std::move(g));
    AssembledFunction caller;
    caller.symbol = SymbolId{2};
    caller.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};   // call rel32 ; ret
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{3};                  // the reference row of the NAME `f`
    rel.kind   = RelocationKind{1};            // rel32 on x86_64
    rel.addend = 0;
    caller.relocations.push_back(rel);
    m.functions.push_back(std::move(caller));
    m.expectedFuncCount = m.functions.size();
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, prefix + "g", gBinding, SymbolVisibility::Default});
    ModuleSymbol alias{SymbolId{1}, prefix + "f", fBinding, SymbolVisibility::Default};
    alias.referencedByName = true;
    // A weak alias is the OVERRIDABLE kind of weak definition — C's `weak`, whose
    // linkage row states that kind (`c.lang.json`, `"weak"`), on every format; the
    // formats that read a kind (PE) need it to know it yields to a strong one.
    if (fBinding == SymbolBinding::Weak) alias.weakKind = WeakDefinitionKind::Overridable;
    m.symbols.push_back(alias);
    m.symbols.push_back(ModuleSymbol{SymbolId{2}, prefix + "main", SymbolBinding::Global, SymbolVisibility::Default});
    ExternImport ref;
    ref.symbol      = SymbolId{3};
    ref.mangledName = prefix + "f";
    m.externImports.push_back(std::move(ref));
    m.userEntrySymbol = SymbolId{2};
    return m;
}

// A second unit that defines the alias's NAME strong, as a body of its own.
[[nodiscard]] AssembledModule rivalUnit(std::string const& prefix) {
    AssembledModule m;
    m.cuId = CompilationUnitId{2};
    AssembledFunction f;
    f.symbol = SymbolId{1};
    f.bytes  = kRivalBody;
    m.functions.push_back(std::move(f));
    m.expectedFuncCount = 1;
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, prefix + "f", SymbolBinding::Global, SymbolVisibility::Default});
    return m;
}

[[nodiscard]] std::vector<std::size_t> findAll(std::vector<std::uint8_t> const& hay,
                                               std::vector<std::uint8_t> const& needle) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size() && match; ++j) match = hay[i + j] == needle[j];
        if (match) out.push_back(i);
    }
    return out;
}

// The FILE OFFSET a `call rel32 ; ret` lands on, for every such call in the image.
// Both ends are in the one code section, whose file bytes are laid out as its
// addresses are, so the displacement is a distance between file offsets too.
[[nodiscard]] std::vector<std::int64_t> callTargets(std::vector<std::uint8_t> const& b) {
    std::vector<std::int64_t> out;
    for (std::size_t p = 0; p + 6 <= b.size(); ++p) {
        if (b[p] != 0xE8 || b[p + 5] != 0xC3) continue;
        std::uint32_t d = 0;
        for (int i = 0; i < 4; ++i) d |= static_cast<std::uint32_t>(b[p + 1 + i]) << (8 * i);
        out.push_back(static_cast<std::int64_t>(p) + 5 + static_cast<std::int32_t>(d));
    }
    return out;
}

[[nodiscard]] bool someCallLandsOn(std::vector<std::uint8_t> const& image, std::vector<std::uint8_t> const& body) {
    auto const at = findAll(image, body);
    if (at.size() != 1u) return false;
    for (std::int64_t t : callTargets(image)) {
        if (t == static_cast<std::int64_t>(at.front())) return true;
    }
    return false;
}

[[nodiscard]] std::optional<AssembledModule> readBack(std::string_view fmt, LinkedImage const& obj,
                                                      Loaded const& L, DiagnosticReporter& rep) {
    if (fmt.starts_with("macho")) return macho::readRelocatableObject(obj.bytes, *L.target, *L.format, rep);
    if (fmt.starts_with("pe")) return pe::readRelocatableObject(obj.bytes, *L.target, *L.format, rep);
    return elf::readRelocatableObject(obj.bytes, *L.target, *L.format, rep);
}

}  // namespace

// An image of the one unit: the call through the alias lands on the aliased body,
// for a strong and a weak alias, a global and a `static` target.
TEST(CompiledSymbolAlias, AnImageCallsTheAliasedBodyThroughTheAlias) {
    for (Format const& fmt : kFormats) {
        for (SymbolBinding const gb : {SymbolBinding::Global, SymbolBinding::Local}) {
            for (SymbolBinding const fb : {SymbolBinding::Global, SymbolBinding::Weak}) {
                SCOPED_TRACE(std::string{fmt.image} + ", target " + (gb == SymbolBinding::Local ? "static" : "global")
                             + ", alias " + (fb == SymbolBinding::Weak ? "weak" : "strong"));
                auto const L = load("x86_64", fmt.image);
                ASSERT_TRUE(L.target && L.format);
                std::vector<AssembledModule> mods{compiledAliasUnit(fmt.prefix, gb, fb)};
                DiagnosticReporter rep;
                auto const img = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep,
                                              kImage);
                ASSERT_TRUE(img.ok()) << diagnosticsOf(rep);
                EXPECT_EQ(rep.errorCount(), 0u) << diagnosticsOf(rep);
                EXPECT_TRUE(someCallLandsOn(img.bytes, kAliasedBody))
                    << "no `call rel32` of the image lands on the aliased body: the reference to the alias's name "
                       "did not bind to the body the alias names";
            }
        }
    }
}

// A relocatable object of the one unit carries both names on the one body, each
// with its own binding, and its call still means the NAME `f` — read back by the
// format's own reader. Linked into an image, the object's call lands on the body.
TEST(CompiledSymbolAlias, AnObjectCarriesBothNamesOnOneBodyAndItsCallMeansTheAlias) {
    for (Format const& fmt : kFormats) {
        for (SymbolBinding const fb : {SymbolBinding::Global, SymbolBinding::Weak}) {
            SCOPED_TRACE(std::string{fmt.object} + ", alias " + (fb == SymbolBinding::Weak ? "weak" : "strong"));
            std::string const f = std::string{fmt.prefix} + "f";
            std::string const g = std::string{fmt.prefix} + "g";
            auto const L = load("x86_64", fmt.object);
            ASSERT_TRUE(L.target && L.format);
            std::vector<AssembledModule> mods{compiledAliasUnit(fmt.prefix, SymbolBinding::Global, fb)};
            DiagnosticReporter rep;
            auto const obj = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
            ASSERT_TRUE(obj.ok()) << diagnosticsOf(rep);
            DiagnosticReporter readRep;
            auto const back = readBack(fmt.object, obj, L, readRep);
            ASSERT_TRUE(back.has_value()) << diagnosticsOf(readRep);
            ModuleSymbol const* fRow = nullptr;
            ModuleSymbol const* gRow = nullptr;
            for (auto const& s : back->symbols) {
                if (s.name == f) fRow = &s;
                if (s.name == g) gRow = &s;
            }
            ASSERT_NE(gRow, nullptr) << "the target's own name";
            ASSERT_NE(fRow, nullptr) << "the ALIAS's name is not defined by the object";
            EXPECT_EQ(fRow->symbol, gRow->symbol) << "the two names are not one body";
            EXPECT_EQ(fRow->binding, fb);
            EXPECT_EQ(gRow->binding, SymbolBinding::Global);
            // The call through a WEAK alias names `f`: a reference row of the name,
            // or the name's own row as a reader states a weak name it keeps a
            // reference on (`referencedByName`) — never the body alone. (Through a
            // STRONG alias the two names are one definition nothing can replace, and
            // a reader that puts both on one id cannot tell them apart: not asked.)
            std::size_t throughTheName = 0;
            std::size_t calls          = 0;
            for (auto const& fn : back->functions) {
                for (auto const& rel : fn.relocations) {
                    ++calls;
                    bool named = false;
                    for (auto const& e : back->externImports) {
                        named = named || (e.symbol == rel.target && e.mangledName == f);
                    }
                    for (auto const& s : back->symbols) {
                        named = named || (s.symbol == rel.target && s.name == f && s.referencedByName);
                    }
                    throughTheName += named ? 1u : 0u;
                }
            }
            EXPECT_EQ(calls, 1u);
            if (fb == SymbolBinding::Weak) {
                EXPECT_EQ(throughTheName, 1u) << "the object's call through a WEAK alias must stay on the NAME, or a "
                                                 "later link cannot give the name to another unit's definition";
            }
            // The object, linked alone into an image, calls the body.
            auto const I = load("x86_64", fmt.image);
            ASSERT_TRUE(I.target && I.format);
            AssembledModule again = *back;
            again.cuId = CompilationUnitId{1};
            for (auto const& s : again.symbols) {
                if (s.name == std::string{fmt.prefix} + "main") again.userEntrySymbol = s.symbol;
            }
            std::vector<AssembledModule> one{again};
            DiagnosticReporter linkRep;
            auto const img = linker::link(std::span<AssembledModule const>{one}, *I.target, *I.format, linkRep, kImage);
            ASSERT_TRUE(img.ok()) << diagnosticsOf(linkRep);
            EXPECT_TRUE(someCallLandsOn(img.bytes, kAliasedBody)) << "the object's call does not reach the body";
        }
    }
}

// P69 (lane `cs`, fold f26b): a STRONG alias of a WEAK definition of the SELECT-ANY kind — `__declspec(selectany)`'s
// kind, which PE writes as a COMDAT section of its own. The alias is a strong name on the definition's body: an
// external label in that COMDAT section on PE (what clang-cl writes for a select-any definition with a global alias,
// ✔MEASURED by lane `xa`, 2026-10-20), a global symbol at the body's address on ELF and Mach-O. A relocatable object
// of every format, read back by the format's own reader, carries the alias as a name of the very body the
// definition's name is WEAK on, and the object, linked alone into an image, calls the body through the alias. What
// the alias reads back as is the FORMAT's: ELF and Mach-O state a binding per symbol, so it is GLOBAL there; COFF
// states a COMDAT section's policy on the SECTION, and every external name in the section is coalesced with it — kept
// with the copy the link keeps, gone with one it discards — so DSS's COFF reader gives the label the section's
// binding, WEAK, as it gives clang-cl's (`coff_object_reader.cpp`, GATE 3). (The PE object writer refused the unit —
// "an alias that must NOT yield" — until this fold.)
TEST(CompiledSymbolAlias, AStrongAliasOfASelectAnyDefinitionIsANameOfItsBody) {
    struct ReadBack {
        char const*   object;
        SymbolBinding alias;
    };
    constexpr ReadBack kAliasReadsBackAs[] = {
        {"elf64-x86_64-linux", SymbolBinding::Global},
        {"pe64-x86_64-windows", SymbolBinding::Weak},
        {"macho64-x86_64-darwin", SymbolBinding::Global},
    };
    for (Format const& fmt : kFormats) {
        SCOPED_TRACE(fmt.object);
        SymbolBinding expected = SymbolBinding::Local;   // no row: the assertion below names the format
        for (ReadBack const& r : kAliasReadsBackAs) {
            if (std::string_view{r.object} == fmt.object) expected = r.alias;
        }
        ASSERT_NE(expected, SymbolBinding::Local) << "no read-back binding is stated for " << fmt.object;
        std::string const f = std::string{fmt.prefix} + "f";
        std::string const g = std::string{fmt.prefix} + "g";
        auto const L = load("x86_64", fmt.object);
        ASSERT_TRUE(L.target && L.format);
        AssembledModule unit = compiledAliasUnit(fmt.prefix, SymbolBinding::Weak, SymbolBinding::Global);
        for (auto& s : unit.symbols) {
            if (s.name == g) s.weakKind = WeakDefinitionKind::SelectAny;
        }
        std::vector<AssembledModule> mods{unit};
        DiagnosticReporter rep;
        auto const obj = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
        ASSERT_TRUE(obj.ok()) << diagnosticsOf(rep);
        EXPECT_EQ(rep.errorCount(), 0u) << diagnosticsOf(rep);
        DiagnosticReporter readRep;
        auto const back = readBack(fmt.object, obj, L, readRep);
        ASSERT_TRUE(back.has_value()) << diagnosticsOf(readRep);
        ModuleSymbol const* fRow = nullptr;
        ModuleSymbol const* gRow = nullptr;
        for (auto const& s : back->symbols) {
            if (s.name == f) fRow = &s;
            if (s.name == g) gRow = &s;
        }
        ASSERT_NE(fRow, nullptr) << "the strong alias is not a record of the object";
        ASSERT_NE(gRow, nullptr) << "the select-any definition is not a record of the object";
        EXPECT_EQ(fRow->binding, expected) << "the alias was not read back as its format states a name of the body";
        EXPECT_EQ(gRow->binding, SymbolBinding::Weak) << "the definition lost its weakness";
        EXPECT_EQ(fRow->symbol, gRow->symbol) << "the two names are not one body";
        // The object, linked alone into an image, calls the body through the strong name.
        auto const I = load("x86_64", fmt.image);
        ASSERT_TRUE(I.target && I.format);
        AssembledModule again = *back;
        again.cuId = CompilationUnitId{1};
        for (auto const& s : again.symbols) {
            if (s.name == std::string{fmt.prefix} + "main") again.userEntrySymbol = s.symbol;
        }
        std::vector<AssembledModule> one{again};
        DiagnosticReporter linkRep;
        auto const img = linker::link(std::span<AssembledModule const>{one}, *I.target, *I.format, linkRep, kImage);
        ASSERT_TRUE(img.ok()) << diagnosticsOf(linkRep);
        EXPECT_TRUE(someCallLandsOn(img.bytes, kAliasedBody)) << "the object's call does not reach the body";
    }
}

// A WEAK alias gives its name up to another unit's strong definition, and the
// unit's OWN call follows the name there (what gcc, clang and MinGW gcc do: the
// alias is a weak definition of `f`). What becomes of the aliased body is the
// format's: where its document keeps a superseded definition's bytes (ELF, PE) the
// body stays under its own name `g`; where it replaces one whole (Mach-O: Apple's
// ld coalesces the atom, `g` then names the winner) the body is not in the image.
// ⚠ A COMPILED alias never reaches this on Mach-O — its documents refuse an alias
// by name — but the link tier takes the hand-built module whole, so it states the
// format's answer for it too.
TEST(CompiledSymbolAlias, AWeakAliasYieldsItsNameAndTheUnitsCallFollowsTheName) {
    for (Format const& fmt : kFormats) {
        SCOPED_TRACE(fmt.image);
        auto const L = load("x86_64", fmt.image);
        ASSERT_TRUE(L.target && L.format);
        // The format's own statement, read — so the expectation below is the document's, and a document that
        // changed its answer fails here by name rather than as a missing body.
        ASSERT_EQ(L.format->supersededDefinition().answerFor(WeakDefinitionKind::Overridable),
                  std::optional{fmt.overridden})
            << "the document's answer for a definition that lost an overridable weak name";
        std::size_t const bodiesKept = fmt.overridden == SupersededDefinition::KeepsItsBytes ? 1u : 0u;
        for (bool rivalFirst : {false, true}) {
            SCOPED_TRACE(rivalFirst ? "the rival unit first" : "the alias's unit first");
            std::vector<AssembledModule> mods{compiledAliasUnit(fmt.prefix, SymbolBinding::Global, SymbolBinding::Weak),
                                              rivalUnit(fmt.prefix)};
            if (rivalFirst) std::swap(mods[0], mods[1]);
            DiagnosticReporter rep;
            auto const img = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep, kImage);
            ASSERT_TRUE(img.ok()) << diagnosticsOf(rep);
            EXPECT_TRUE(someCallLandsOn(img.bytes, kRivalBody))
                << "the unit's call through its weak alias did not follow the name to the strong definition";
            EXPECT_FALSE(someCallLandsOn(img.bytes, kAliasedBody)) << "the call still reaches the aliased body";
            EXPECT_EQ(findAll(img.bytes, kAliasedBody).size(), bodiesKept)
                << (bodiesKept == 1u ? "the aliased body is gone with the weak name, where the format keeps a "
                                       "superseded definition's bytes under its other names"
                                     : "the aliased body stands under its other name, where the format replaces a "
                                       "superseded definition whole");
        }
    }
}

// A STRONG alias beside another unit's strong definition of the same name is two
// definitions of one name: refused by name, as every reference linker refuses it.
TEST(CompiledSymbolAlias, AStrongAliasBesideAnotherStrongDefinitionIsRefusedByName) {
    for (Format const& fmt : kFormats) {
        SCOPED_TRACE(fmt.image);
        auto const L = load("x86_64", fmt.image);
        ASSERT_TRUE(L.target && L.format);
        std::vector<AssembledModule> mods{compiledAliasUnit(fmt.prefix, SymbolBinding::Global, SymbolBinding::Global),
                                          rivalUnit(fmt.prefix)};
        DiagnosticReporter rep;
        auto const img = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep, kImage);
        EXPECT_FALSE(img.ok());
        EXPECT_TRUE(reported(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits)) << diagnosticsOf(rep);
    }
}

namespace {

// THE TWO BINDINGS DIFFER WITH NO WEAK SIDE: `g` module-private and its alias `f` global
// (`static int g(void){…} int f(void) __attribute__((alias("g")));`), or the reverse. `main` calls the
// GLOBAL one of the two names through a reference row of it.
[[nodiscard]] AssembledModule mixedBindingUnit(std::string const& prefix, bool aliasIsGlobal) {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    AssembledFunction g;
    g.symbol = SymbolId{1};
    g.bytes  = kAliasedBody;
    m.functions.push_back(std::move(g));
    AssembledFunction caller;
    caller.symbol = SymbolId{2};
    caller.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};   // call rel32 ; ret
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{3};
    rel.kind   = RelocationKind{1};
    rel.addend = 0;
    caller.relocations.push_back(rel);
    m.functions.push_back(std::move(caller));
    m.expectedFuncCount = m.functions.size();
    SymbolBinding const gBinding = aliasIsGlobal ? SymbolBinding::Local : SymbolBinding::Global;
    SymbolBinding const fBinding = aliasIsGlobal ? SymbolBinding::Global : SymbolBinding::Local;
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, prefix + "g", gBinding, SymbolVisibility::Default});
    ModuleSymbol alias{SymbolId{1}, prefix + "f", fBinding, SymbolVisibility::Default};
    alias.referencedByName = aliasIsGlobal;
    m.symbols.push_back(alias);
    m.symbols.push_back(ModuleSymbol{SymbolId{2}, prefix + "main", SymbolBinding::Global, SymbolVisibility::Default});
    ExternImport ref;
    ref.symbol      = SymbolId{3};
    ref.mangledName = prefix + (aliasIsGlobal ? "f" : "g");
    m.externImports.push_back(std::move(ref));
    m.userEntrySymbol = SymbolId{2};
    return m;
}

}  // namespace

// A GLOBAL alias of a MODULE-PRIVATE body, and a MODULE-PRIVATE alias of a GLOBAL one: neither binding is weak, so
// there is no coalescing policy for the two names to differ in. A relocatable object of every format — read back by
// the format's own reader — carries the GLOBAL name of the pair as an external record of the body, and the
// module-private one never as an exported name (a writer writes an object's external-linkage names; ELF names a
// module-private body `sym_<id>` there, and no writer writes a module-private second name); the object, linked into
// an image, calls the body through the global name. (mingw-w64 gcc 13.2.0 builds and runs the first on PE:
// ✔MEASURED, lane `cs` probe al2 cell 3; P69, the PE writer refused it until its alias arm took the pair.)
TEST(CompiledSymbolAlias, AnAliasWhoseBindingDiffersFromItsTargetsWithNoWeakSideIsItsOwnRecord) {
    for (Format const& fmt : kFormats) {
        for (bool const aliasIsGlobal : {true, false}) {
            SCOPED_TRACE(std::string{fmt.object} + (aliasIsGlobal ? ", a global alias of a static body"
                                                                  : ", a static alias of a global body"));
            std::string const f = std::string{fmt.prefix} + "f";
            std::string const g = std::string{fmt.prefix} + "g";
            auto const L = load("x86_64", fmt.object);
            ASSERT_TRUE(L.target && L.format);
            std::vector<AssembledModule> mods{mixedBindingUnit(fmt.prefix, aliasIsGlobal)};
            DiagnosticReporter rep;
            auto const obj = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
            ASSERT_TRUE(obj.ok()) << diagnosticsOf(rep);
            EXPECT_EQ(rep.errorCount(), 0u) << diagnosticsOf(rep);
            DiagnosticReporter readRep;
            auto const back = readBack(fmt.object, obj, L, readRep);
            ASSERT_TRUE(back.has_value()) << diagnosticsOf(readRep);
            std::string const globalName  = aliasIsGlobal ? f : g;
            std::string const privateName = aliasIsGlobal ? g : f;
            ModuleSymbol const* globalRow = nullptr;
            for (auto const& s : back->symbols)
                if (s.name == globalName) globalRow = &s;
            ASSERT_NE(globalRow, nullptr) << "the global name of the pair is not a record of the object";
            EXPECT_EQ(globalRow->binding, SymbolBinding::Global);
            for (auto const& s : back->symbols) {
                if (s.name != privateName) continue;
                EXPECT_EQ(s.binding, SymbolBinding::Local) << "the module-private name is exported";
                EXPECT_EQ(s.symbol, globalRow->symbol) << "the two names are not one body";
            }
            // The object, linked alone into an image, calls the body through the global name.
            auto const I = load("x86_64", fmt.image);
            ASSERT_TRUE(I.target && I.format);
            AssembledModule again = *back;
            again.cuId = CompilationUnitId{1};
            for (auto const& s : again.symbols) {
                if (s.name == std::string{fmt.prefix} + "main") again.userEntrySymbol = s.symbol;
            }
            std::vector<AssembledModule> one{again};
            DiagnosticReporter linkRep;
            auto const img = linker::link(std::span<AssembledModule const>{one}, *I.target, *I.format, linkRep, kImage);
            ASSERT_TRUE(img.ok()) << diagnosticsOf(linkRep);
            EXPECT_TRUE(someCallLandsOn(img.bytes, kAliasedBody)) << "the object's call does not reach the body";
        }
    }
}
