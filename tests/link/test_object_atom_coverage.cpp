// The relocatable-object readers' ATOM-COVERAGE guard --
// D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM.
//
// ★★★ WHAT THIS FILE PINS, AND WHY THE PINS ARE SHAPED THIS WAY.
//
// The COFF and Mach-O readers classify every NON-EXTERNAL defined section
// symbol as an interior block label (an `&&label`) rather than an atom
// boundary. That is right for a label and wrong for a whole file-local
// (`static`) function -- and IMAGE_SYMBOL / nlist_64 carry no size field, so
// the two shapes are the same three numbers. ELF is unaffected: `st_size` lets
// it slice any STT_FUNC with a non-empty extent regardless of BINDING.
//
// The consequence had a loud half and a silent half, and only the silent half
// needed a new instrument:
//   * a CALLED file-local function failed loud downstream (`K_SymbolUndefined`
//     out of the cross-CU resolve -- nothing defines the relocation's target);
//   * an UNCALLED one linked GREEN with its bytes dropped.
//
// ⚠ THE SILENT HALF IS WHY EVERY PIN HERE ASSERTS A SIZE. A boolean "the link
// succeeded" cannot see this defect -- the link DID succeed, that was the
// problem. What distinguishes the two worlds is a NUMBER: whether the section
// the reader claims to have reconstructed is the whole section. So the central
// assertion is `sum(reconstructed atom bytes) == the object's text section
// size`, which reds in the exact direction the defect moves and would have
// caught it before the guard existed.
//
// ⓘ WHAT IS NOT FIXED HERE, deliberately. The CLASSIFICATION is unchanged --
// the Mach-O half of that needs a wire-format change. These tests pin the
// CONVERSION of a silent byte loss into a loud refusal that NAMES the symbol,
// which is the whole of this cycle's claim.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/elf.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/macho.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/format/pe.hpp"
#include "link/object_format_schema.hpp"

#include "link_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using ::dss::link_format::test::readU32LE;
using ::dss::link_format::test::readU64LE;

namespace {

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShipped(std::string_view targetName,
                                 std::string_view formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped(targetName);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped target " << targetName << " failed";
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped format " << formatName << " failed";
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

[[nodiscard]] bool sawCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) if (d.code == code) return true;
    return false;
}

[[nodiscard]] std::string detailFor(DiagnosticReporter const& rep,
                                    DiagnosticCode           code) {
    for (auto const& d : rep.all()) if (d.code == code) return d.actual;
    return {};
}

// -- Text-section SIZE, read back out of each format's own headers ----------
//
// The pins below compare "how many bytes the section holds" against "how many
// bytes the reader reconstructed", so the first number has to come from the
// OBJECT, never from the module that produced it -- otherwise both sides of the
// comparison would be the same belief and the assertion would be vacuous.

// ELF64: e_shoff/e_shentsize/e_shnum/e_shstrndx -> the `.text` Shdr's sh_size.
[[nodiscard]] std::optional<std::uint64_t>
elfTextSize(std::vector<std::uint8_t> const& b) {
    if (b.size() < 64) return std::nullopt;
    std::span<std::uint8_t const> const s{b.data(), b.size()};
    std::uint64_t const shoff     = readU64LE(s, 0x28);
    std::uint16_t const shentsize = static_cast<std::uint16_t>(readU32LE(s, 0x3A) & 0xFFFFu);
    std::uint16_t const shnum     = static_cast<std::uint16_t>(readU32LE(s, 0x3C) & 0xFFFFu);
    std::uint16_t const shstrndx  = static_cast<std::uint16_t>(readU32LE(s, 0x3E) & 0xFFFFu);
    if (shstrndx >= shnum) return std::nullopt;
    std::size_t const strHdr = static_cast<std::size_t>(shoff + shstrndx * shentsize);
    if (strHdr + 64 > b.size()) return std::nullopt;
    std::uint64_t const strOff = readU64LE(s, strHdr + 24);   // sh_offset
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::size_t const h = static_cast<std::size_t>(shoff + i * shentsize);
        if (h + 64 > b.size()) return std::nullopt;
        std::uint32_t const nameOff = readU32LE(s, h);        // sh_name
        std::size_t const   n       = static_cast<std::size_t>(strOff + nameOff);
        if (n >= b.size()) continue;
        if (std::strcmp(reinterpret_cast<char const*>(&b[n]), ".text") == 0) {
            return readU64LE(s, h + 32);                      // sh_size
        }
    }
    return std::nullopt;
}

// COFF: NumberOfSections -> the `.text` IMAGE_SECTION_HEADER's SizeOfRawData.
[[nodiscard]] std::optional<std::uint64_t>
coffTextSize(std::vector<std::uint8_t> const& b) {
    if (b.size() < 20) return std::nullopt;
    std::span<std::uint8_t const> const s{b.data(), b.size()};
    std::uint16_t const nsec = static_cast<std::uint16_t>(readU32LE(s, 2) & 0xFFFFu);
    for (std::uint16_t i = 0; i < nsec; ++i) {
        std::size_t const h = 20u + static_cast<std::size_t>(i) * 40u;
        if (h + 40 > b.size()) return std::nullopt;
        char name[9] = {};
        std::memcpy(name, &b[h], 8);
        if (std::string_view{name} == ".text") return readU32LE(s, h + 16);
    }
    return std::nullopt;
}

// Mach-O MH_OBJECT: an object carries ONE anonymous catch-all segment whose
// LC_SEGMENT_64 segname is EMPTY, so the section is found by walking the
// section_64 records' OWN names (`macho_test_support.hpp::findSection` resolves
// the segment by the COMMAND's name and is image-shaped -- it would silently
// return nullopt here).
[[nodiscard]] std::optional<std::uint64_t>
machoTextSize(std::vector<std::uint8_t> const& b) {
    if (b.size() < 32) return std::nullopt;
    std::span<std::uint8_t const> const s{b.data(), b.size()};
    std::uint32_t const ncmds = readU32LE(s, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > b.size()) return std::nullopt;
        std::uint32_t const cmd     = readU32LE(s, off);
        std::uint32_t const cmdsize = readU32LE(s, off + 4);
        if (cmdsize == 0) return std::nullopt;
        if (cmd == 0x19u) {   // LC_SEGMENT_64
            if (off + 72 > b.size()) return std::nullopt;
            std::uint32_t const nsects = readU32LE(s, off + 64);
            for (std::uint32_t k = 0; k < nsects; ++k) {
                std::size_t const sec = off + 72u + static_cast<std::size_t>(k) * 80u;
                if (sec + 80 > b.size()) return std::nullopt;
                char name[17] = {};
                std::memcpy(name, &b[sec], 16);
                if (std::string_view{name} == "__text") {
                    return readU64LE(s, sec + 40);   // section_64.size
                }
            }
        }
        off += cmdsize;
    }
    return std::nullopt;
}

// -- The subject module ------------------------------------------------------
//
// `helper` is LOCAL and LEADING; `entry` is GLOBAL and follows it. That order is
// the one that loses bytes: on COFF/Mach-O the demoted `helper` starts no atom,
// so `entry`'s atom begins at `helper`'s successor offset and `helper`'s bytes
// belong to nothing.
//
// ⚠ THE TRAILING ORDER IS A DIFFERENT (uncaught) DEFECT and this file does not
// claim it: a local function AFTER a global one falls inside the preceding
// atom, which runs to the end of the section, so its bytes ride along in the
// wrong atom instead of vanishing. Distinguishing that from a genuine interior
// label needs the size a non-external symbol does not carry.
constexpr std::size_t kHelperLen = 8;   // 8 x RET -- a distinct, countable extent
constexpr std::size_t kEntryLen  = 4;

[[nodiscard]] AssembledModule subjectModule(bool withLocalHelper) {
    AssembledModule mod;
    if (withLocalHelper) {
        AssembledFunction helper;
        helper.symbol = SymbolId{1};
        helper.bytes.assign(kHelperLen, 0xC3);
        mod.functions.push_back(std::move(helper));
        mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "helper",
                                           SymbolBinding::Local,
                                           SymbolVisibility::Default});
    }
    AssembledFunction entry;
    entry.symbol = SymbolId{2};
    entry.bytes.assign(kEntryLen, 0xC3);
    mod.functions.push_back(std::move(entry));
    mod.symbols.push_back(ModuleSymbol{SymbolId{2}, "entry", SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.expectedFuncCount = mod.functions.size();
    return mod;
}

// Each writer's `encode` also accepts an `ImageRequest` (defaulted), so its
// address does not have the 4-argument shape the leg tables want. Thin
// adapters, one per format -- deliberately NOT a `std::function`: a function
// pointer keeps each leg row a compile-time constant and keeps the tables
// readable as data.
using EncodeFn = std::vector<std::uint8_t> (*)(AssembledModule const&,
                                               TargetSchema const&,
                                               ObjectFormatSchema const&,
                                               DiagnosticReporter&);
using ReadFn = std::optional<AssembledModule> (*)(std::span<std::uint8_t const>,
                                                  TargetSchema const&,
                                                  ObjectFormatSchema const&,
                                                  DiagnosticReporter&,
                                                  CompilationUnitId);

std::vector<std::uint8_t> encodeElf(AssembledModule const& m, TargetSchema const& t,
                                    ObjectFormatSchema const& f, DiagnosticReporter& r) {
    return elf::encode(m, t, f, r);
}
std::vector<std::uint8_t> encodePe(AssembledModule const& m, TargetSchema const& t,
                                   ObjectFormatSchema const& f, DiagnosticReporter& r) {
    return pe::encode(m, t, f, r);
}
std::vector<std::uint8_t> encodeMacho(AssembledModule const& m, TargetSchema const& t,
                                      ObjectFormatSchema const& f, DiagnosticReporter& r) {
    return macho::encode(m, t, f, r);
}

// ★ THE NAME IN THE DIAGNOSTIC IS THE OBJECT'S SPELLING, NOT THE SOURCE'S, and
// that is the only spelling that can be right. DSS's writers rename an
// INTERNAL-LINKAGE function to a synthesized `sym_<id>`
// (D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION -- a sibling `.o`'s
// unrelated local must never bind to it), so a module-level `helper` reaches the
// object as `sym_1`. A triager greps `nm` output, which shows the object's
// spelling; echoing a source name the symbol table does not contain would send
// them looking for something that is not there. Derived from the id rather than
// written as a literal so the coupling is visible.
[[nodiscard]] std::string dssLocalSpelling(SymbolId id) {
    return "sym_" + std::to_string(id.v);
}

// -- A minimal hand-rolled COFF `.obj` --------------------------------------
//
// Pins that the message carries a name THIS TEST CHOSE, not merely the
// synthesized pattern above -- and stands in for a foreign (cl.exe / clang-cl)
// object, where a file-local function keeps its real source name. Deliberately
// hand-rolled rather than produced by `pe::encode`: DSS's own writer cannot emit
// a STATIC symbol carrying an arbitrary name, which is exactly the shape under
// test. Mirrors the builder in `test_coff_object_reader.cpp`; kept local because
// this file needs only the two-symbol single-section case.
constexpr std::uint32_t kScnText       = 0x60500020u;  // CODE|ALIGN16|EXEC|READ
constexpr std::uint8_t  kClassExternal = 2;
constexpr std::uint8_t  kClassStatic   = 3;

void emitU16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}
void emitU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
}
void emitName8(std::vector<std::uint8_t>& b, std::string const& n) {
    for (std::size_t i = 0; i < 8u; ++i)
        b.push_back(i < n.size() ? static_cast<std::uint8_t>(n[i]) : 0u);
}

struct BSym {
    std::string   name;      // <= 8 bytes (inline form)
    std::uint32_t value = 0;
    std::uint8_t  storage = kClassExternal;
};

// One `.text` section holding `body`, plus `syms` (all defined in it, no aux
// records, no relocations, every name inline).
[[nodiscard]] std::vector<std::uint8_t>
buildOneSectionCoff(std::vector<std::uint8_t> const& body,
                    std::vector<BSym> const&         syms) {
    std::uint32_t const bodyOff   = 20u + 40u;
    std::uint32_t const symTabPtr = bodyOff + static_cast<std::uint32_t>(body.size());
    std::vector<std::uint8_t> out;
    emitU16(out, 0x8664u);                                   // Machine AMD64
    emitU16(out, 1u);                                        // NumberOfSections
    emitU32(out, 0u);                                        // TimeDateStamp
    emitU32(out, symTabPtr);
    emitU32(out, static_cast<std::uint32_t>(syms.size()));
    emitU16(out, 0u);                                        // SizeOfOptionalHeader
    emitU16(out, 0u);                                        // Characteristics
    emitName8(out, ".text");
    emitU32(out, 0u);                                        // VirtualSize
    emitU32(out, 0u);                                        // VirtualAddress
    emitU32(out, static_cast<std::uint32_t>(body.size()));   // SizeOfRawData
    emitU32(out, body.empty() ? 0u : bodyOff);               // PointerToRawData
    emitU32(out, 0u);                                        // PointerToRelocations
    emitU32(out, 0u);                                        // PointerToLinenumbers
    emitU16(out, 0u);                                        // NumberOfRelocations
    emitU16(out, 0u);                                        // NumberOfLinenumbers
    emitU32(out, kScnText);                                  // Characteristics
    out.insert(out.end(), body.begin(), body.end());
    for (auto const& sy : syms) {
        emitName8(out, sy.name);
        emitU32(out, sy.value);
        emitU16(out, 1u);            // SectionNumber -> .text
        emitU16(out, 0x20u);         // Type: DTYPE_FUNCTION
        out.push_back(sy.storage);
        out.push_back(0u);           // NumberOfAuxSymbols
    }
    emitU32(out, 4u);                // string table: size prefix only
    return out;
}

[[nodiscard]] std::size_t reconstructedTextBytes(AssembledModule const& m) {
    std::size_t total = 0;
    for (auto const& f : m.functions) total += f.bytes.size();
    return total;
}

} // namespace

// ============================================================================
// PIN 1 -- THE GUARD FIRES, AND IT NAMES THE SYMBOL.
//
// The whole failure mode was that nobody could tell WHICH symbol vanished, so
// asserting the code alone would leave the property that matters unpinned.
// ============================================================================

TEST(ObjectAtomCoverage, CoffUncoveredDefinedSymbolFailsLoudNamingIt) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter wrep;
    auto obj = pe::encode(subjectModule(/*withLocalHelper=*/true), *loaded.target,
                          *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u) << "the WRITER accepts this module -- the "
                                        "object is well formed; the reader is "
                                        "the half that cannot represent it";
    ASSERT_FALSE(obj.empty());

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value())
        << "a file-local function that starts no atom must REFUSE, not "
           "reconstruct a module missing its bytes";
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_ObjectReaderSymbolBodyDropped));
    std::string const detail =
        detailFor(rep, DiagnosticCode::F_ObjectReaderSymbolBodyDropped);
    EXPECT_NE(detail.find(dssLocalSpelling(SymbolId{1})), std::string::npos)
        << "the message must NAME the symbol whose bytes would be dropped, as "
           "the OBJECT spells it; got: " << detail;
    EXPECT_NE(detail.find(".text"), std::string::npos)
        << "...and the section it lives in; got: " << detail;
}

TEST(ObjectAtomCoverage, MachoUncoveredDefinedSymbolFailsLoudNamingIt) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter wrep;
    auto obj = macho::encode(subjectModule(/*withLocalHelper=*/true), *loaded.target,
                             *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_FALSE(obj.empty());

    DiagnosticReporter rep;
    auto got = macho::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_ObjectReaderSymbolBodyDropped));
    std::string const detail =
        detailFor(rep, DiagnosticCode::F_ObjectReaderSymbolBodyDropped);
    // Mach-O C mangling additionally prepends `_`, so the object's spelling is
    // `_sym_1`; the synthesized stem is the identity either way.
    EXPECT_NE(detail.find(dssLocalSpelling(SymbolId{1})), std::string::npos)
        << "the message must NAME the symbol as the OBJECT spells it; got: "
        << detail;
    EXPECT_NE(detail.find("__text"), std::string::npos)
        << "...and the section it lives in; got: " << detail;
}

// ============================================================================
// PIN 2 -- THE SIZE PIN. A reader that returns a module must have reconstructed
// the WHOLE text section; otherwise it must refuse.
//
// ★ THIS IS THE ASSERTION THAT WOULD HAVE CAUGHT THE DEFECT. Before the guard,
// the COFF/Mach-O `withLocalHelper` read returned a module whose function bytes
// summed to kEntryLen while the section held kEntryLen + kHelperLen -- green,
// and short by exactly the dropped function. `EXPECT_TRUE(read.has_value())`
// could not see that; this can.
// ============================================================================

TEST(ObjectAtomCoverage, EveryReaderReconstructsTheWholeTextSectionOrRefuses) {
    struct Leg {
        char const* targetName;
        char const* formatName;
        EncodeFn                     encode;
        ReadFn                       read;
        std::optional<std::uint64_t> (*textSize)(std::vector<std::uint8_t> const&);
        bool localFunctionIsAnAtom;   // ELF: yes (st_size). COFF/Mach-O: no.
    };
    Leg const legs[] = {
        {"x86_64", "elf64-x86_64-linux",   &encodeElf,   &elf::readRelocatableObject,
         &elfTextSize,   /*localFunctionIsAnAtom=*/true},
        {"x86_64", "pe64-x86_64-windows",  &encodePe,    &pe::readRelocatableObject,
         &coffTextSize,  /*localFunctionIsAnAtom=*/false},
        {"arm64",  "macho64-arm64-darwin", &encodeMacho, &macho::readRelocatableObject,
         &machoTextSize, /*localFunctionIsAnAtom=*/false},
    };

    for (auto const& leg : legs) {
        SCOPED_TRACE(leg.formatName);
        auto loaded = loadShipped(leg.targetName, leg.formatName);
        ASSERT_TRUE(loaded.target && loaded.format);

        // -- (a) WITHOUT the file-local function: every reader is whole. --
        DiagnosticReporter wrepA;
        auto objA = leg.encode(subjectModule(/*withLocalHelper=*/false),
                               *loaded.target, *loaded.format, wrepA);
        ASSERT_EQ(wrepA.errorCount(), 0u);
        auto const sizeA = leg.textSize(objA);
        ASSERT_TRUE(sizeA.has_value()) << "the text section must be locatable";

        DiagnosticReporter repA;
        auto gotA = leg.read(objA, *loaded.target, *loaded.format, repA, {});
        ASSERT_TRUE(gotA.has_value()) << "errors=" << repA.errorCount();
        EXPECT_EQ(reconstructedTextBytes(*gotA), *sizeA)
            << "a green read must account for every byte of the section";

        // -- (b) WITH it: the section GREW by exactly the function's extent. --
        DiagnosticReporter wrepB;
        auto objB = leg.encode(subjectModule(/*withLocalHelper=*/true),
                               *loaded.target, *loaded.format, wrepB);
        ASSERT_EQ(wrepB.errorCount(), 0u);
        auto const sizeB = leg.textSize(objB);
        ASSERT_TRUE(sizeB.has_value());
        EXPECT_EQ(*sizeB, *sizeA + kHelperLen)
            << "the WRITER keeps a file-local function -- the bytes ARE in the "
               "object, which is what makes losing them on the way back in a "
               "miscompile rather than an absence";

        DiagnosticReporter repB;
        auto gotB = leg.read(objB, *loaded.target, *loaded.format, repB, {});
        if (leg.localFunctionIsAnAtom) {
            // ELF slices any STT_FUNC with a non-empty extent regardless of
            // BINDING, so it reconstructs the grown section whole. Pinned so the
            // guard cannot regress the one reader that is already correct.
            ASSERT_TRUE(gotB.has_value()) << "errors=" << repB.errorCount();
            EXPECT_EQ(repB.errorCount(), 0u);
            EXPECT_EQ(reconstructedTextBytes(*gotB), *sizeB);
            EXPECT_EQ(reconstructedTextBytes(*gotB),
                      reconstructedTextBytes(*gotA) + kHelperLen)
                << "ELF's reconstruction GROWS with the source -- the exact "
                   "property whose absence was the silent miscompile";
        } else {
            // The demotion is unchanged, so the reader cannot reconstruct the
            // section whole; it must say so rather than return the short module.
            EXPECT_FALSE(gotB.has_value())
                << "a short reconstruction must never be returned green -- it "
                   "would have summed to " << *sizeA << " against a section of "
                << *sizeB;
            EXPECT_TRUE(sawCode(repB, DiagnosticCode::F_ObjectReaderSymbolBodyDropped));
        }
    }
}

// ============================================================================
// PIN 3 -- NO FALSE FIRE ON THE SHAPE THE DEMOTION EXISTS FOR.
//
// A computed-goto `&&label` is a LOCAL defined section symbol too, and it must
// keep reading green -- it lies INSIDE the function that contains it, so the
// enclosing atom covers its offset. That is the whole discrimination the guard
// rests on, with no format test and no name matching, so it is worth its own
// pin: a guard that also refused labels would be an unusable guard, and the
// smallest way to "fix" that would be an escape hatch.
// ============================================================================

TEST(ObjectAtomCoverage, InteriorBlockLabelIsCoveredAndStillReadsGreen) {
    struct Leg {
        char const* targetName;
        char const* formatName;
        EncodeFn    encode;
        ReadFn      read;
    };
    Leg const legs[] = {
        {"x86_64", "elf64-x86_64-linux",   &encodeElf,   &elf::readRelocatableObject},
        {"x86_64", "pe64-x86_64-windows",  &encodePe,    &pe::readRelocatableObject},
        {"arm64",  "macho64-arm64-darwin", &encodeMacho, &macho::readRelocatableObject},
    };

    for (auto const& leg : legs) {
        SCOPED_TRACE(leg.formatName);
        auto loaded = loadShipped(leg.targetName, leg.formatName);
        ASSERT_TRUE(loaded.target && loaded.format);

        AssembledModule mod;
        AssembledFunction entry;
        entry.symbol = SymbolId{2};
        entry.bytes.assign(16, 0xC3);
        // A synthetic per-block local at byte 8 -- strictly INTERIOR to `entry`.
        entry.blockSymbols.push_back(SyntheticBlockSymbol{SymbolId{3}, 8u});
        mod.functions.push_back(std::move(entry));
        mod.symbols = {
            ModuleSymbol{SymbolId{2}, "entry", SymbolBinding::Global,
                         SymbolVisibility::Default},
            ModuleSymbol{SymbolId{3}, "entry_blk8", SymbolBinding::Local,
                         SymbolVisibility::Default},
        };
        mod.expectedFuncCount = 1;

        DiagnosticReporter wrep;
        auto obj = leg.encode(mod, *loaded.target, *loaded.format, wrep);
        ASSERT_EQ(wrep.errorCount(), 0u);

        DiagnosticReporter rep;
        auto got = leg.read(obj, *loaded.target, *loaded.format, rep, {});
        ASSERT_TRUE(got.has_value())
            << "an interior block label is COVERED by its enclosing function's "
               "atom and must not trip the guard; errors=" << rep.errorCount();
        EXPECT_FALSE(sawCode(rep, DiagnosticCode::F_ObjectReaderSymbolBodyDropped));
        EXPECT_EQ(reconstructedTextBytes(*got), 16u);
    }
}

// ============================================================================
// PIN 1b -- A FOREIGN-SHAPED OBJECT: the name in the message is the one the
// SYMBOL TABLE carries, whatever it is.
//
// Pin 1 proves the message names the symbol for an object DSS wrote, where the
// spelling is synthesized. This proves the message is reading the object rather
// than reproducing that pattern: a hand-rolled `.obj` whose STATIC symbol is
// named `helper` -- the shape a real cl.exe / clang-cl object has, where
// a file-local function keeps its source name.
//
// ⓘ The chosen name must be <= 8 bytes: this builder emits the INLINE COFF name
// form, and a longer one is silently TRUNCATED into the 8-byte field rather than
// spilling to the string table.
// ============================================================================

TEST(ObjectAtomCoverage, ForeignShapedCoffNamesTheSymbolTablesOwnSpelling) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    // `.text` = 12 bytes: a STATIC `helper` occupying [0, 8) and an
    // EXTERNAL `entry` at 8. Only `entry` becomes an atom, so [0, 8) is owned by
    // nothing -- eight bytes that would reach no image.
    std::vector<std::uint8_t> body(12, 0xC3);
    auto const obj = buildOneSectionCoff(
        body, {BSym{"helper", 0u, kClassStatic},
               BSym{"entry", 8u, kClassExternal}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(got.has_value());
    ASSERT_TRUE(sawCode(rep, DiagnosticCode::F_ObjectReaderSymbolBodyDropped));
    std::string const detail =
        detailFor(rep, DiagnosticCode::F_ObjectReaderSymbolBodyDropped);
    EXPECT_NE(detail.find("helper"), std::string::npos)
        << "the message must carry the symbol table's OWN name; got: " << detail;

    // ...and the SAME object with the static symbol removed reads green, so the
    // refusal is attributable to that symbol and not to the hand-rolled shape.
    auto const clean = buildOneSectionCoff(body, {BSym{"entry", 0u, kClassExternal}});
    DiagnosticReporter cleanRep;
    auto cleanGot = pe::readRelocatableObject(clean, *loaded.target, *loaded.format,
                                              cleanRep);
    ASSERT_TRUE(cleanGot.has_value()) << "errors=" << cleanRep.errorCount();
    EXPECT_EQ(reconstructedTextBytes(*cleanGot), body.size());
}
