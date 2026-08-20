// The relocatable-object readers' ATOM-COVERAGE guard --
// D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM.
//
// ★★★ WHAT THIS FILE PINS, AND WHY THE PINS ARE SHAPED THIS WAY.
//
// The COFF and Mach-O readers used to classify every NON-EXTERNAL defined
// section symbol as an interior block label (an `&&label`) rather than an atom
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
// ⓘ THE CLASSIFICATION IS NOW FIXED ON BOTH FORMATS, BY DIFFERENT EVIDENCE, AND
// THE ROUTES ARE PINNED SEPARATELY BECAUSE THEY CAN REGRESS SEPARATELY.
//   * COFF reads discriminators that were on the wire all along: IMAGE_SYMBOL's
//     derived type promotes a class-STATIC DTYPE_FUNCTION symbol (PIN 1, PIN
//     1b), and the SECTION's kind promotes a class-STATIC data object. Whatever
//     neither reaches, atom-coverage GEOMETRY recovers (PIN 4).
//   * Mach-O reads the PAIR its format actually defines --
//     `MH_SUBSECTIONS_VIA_SYMBOLS` in the header plus `N_ALT_ENTRY` per symbol
//     -- and takes no geometric fallback, deliberately: with no subsection
//     declaration it has no evidence to reason from, so it demotes and lets the
//     shared refusal speak.
// What survives on both is the refusal itself. On COFF it has become a
// POST-CONDITION -- no object can reach it, only a reader that mis-files its own
// promotion (PIN 5). On Mach-O it is still a live DETECTOR for any object that
// declares no subsections, which is the arm PIN 6 pins.
//
// ⚠ SO THE FILE HOLDS OPPOSITE EXPECTATIONS OF ONE READER ON PURPOSE. An arm
// asserting reconstruction and an arm asserting refusal are not in tension; the
// difference between their objects is one field, and if that stops being the
// difference then exactly one of them reds.

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
#include "link/format/object_atom_coverage.hpp"
#include "link/format/pe.hpp"
#include "link/object_format_schema.hpp"

#include "link_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
// ⚠ THE TRAILING ORDER IS A DIFFERENT DEFECT, and it is the one no COVERAGE
// question can ever see: a local function AFTER a global one falls inside the
// preceding atom, which runs to the end of the section, so its bytes ride along
// in the wrong atom instead of vanishing. Only a CLASSIFICATION reaches it, and
// each reader's own classification is pinned separately for that position --
// `ForeignShapedCoffTrailingLocalFunctionIsNotAbsorbed` here, and
// `TrailingStaticRodataObjectIsNotAbsorbedIntoTheExportedOne` in
// `test_coff_object_reader.cpp` for the data half.
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

// ⚠ `type` IS A REAL FIELD HERE, and it did not used to be. This builder
// hardcoded IMAGE_SYM_DTYPE_FUNCTION on EVERY symbol it emitted, which made its
// objects unable to express the one shape the COFF reader now classifies on --
// so a test written against it could not tell a file-local FUNCTION from a
// file-local DATA object, and would have silently agreed with either reading.
constexpr std::uint16_t kDtypeNone     = 0x00u;
constexpr std::uint16_t kDtypeFunction = 0x20u;

struct BSym {
    std::string   name;      // <= 8 bytes (inline form)
    std::uint32_t value = 0;
    std::uint8_t  storage = kClassExternal;
    std::uint16_t type    = kDtypeFunction;
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
        emitU16(out, sy.type);       // Type: the derived-type hint (0 or 0x20)
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

[[nodiscard]] ModuleSymbol const* symbolNamed(AssembledModule const& m,
                                              std::string_view       name) {
    for (auto const& s : m.symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// The reconstructed function a NAME resolves to, via the module's symbol table
// (the reader keys atoms by SymbolId, not by name).
[[nodiscard]] AssembledFunction const* funcNamed(AssembledModule const& m,
                                                 std::string_view       name) {
    ModuleSymbol const* s = symbolNamed(m, name);
    if (s == nullptr) return nullptr;
    for (auto const& f : m.functions) {
        if (f.symbol.v == s->symbol.v) return &f;
    }
    return nullptr;
}

} // namespace

// ============================================================================
// PIN 1 -- COFF: A FILE-LOCAL FUNCTION IS AN ATOM, AND IT KEEPS ITS INTERNAL
// LINKAGE.
//
// ⓘ THIS PIN USED TO ASSERT THE OPPOSITE. It pinned the REFUSAL: the COFF
// reader classified atom boundaries by EXTERNAL-ness, a file-local function was
// demoted to a bodiless ModuleSymbol, and the atom-coverage guard converted the
// resulting byte loss into a loud error. The reader now reads the discriminator
// that was on the wire all along -- IMAGE_SYMBOL's derived type -- so this
// object RECONSTRUCTS, and the assertions had to move with the behaviour rather
// than the behaviour being trimmed to fit them. The refusal is still pinned, on
// the shape that is still genuinely undecidable (see PIN 1b).
//
// ★ BINDING IS HALF OF THE CLAIM, not a detail. Reconstructing the bytes under
// the WRONG binding is a worse defect than dropping them, because it is silent:
// `Global` makes two members' `sym_<n>` collide (K_SymbolRedefinedAcrossUnits,
// at least loud), and `Weak` makes one member's private function shadow the
// other's and get DROPPED by `isShadowedDuplicate` -- a link that succeeds and
// runs the wrong code. So the binding is asserted exactly, not merely "not
// Global".
// ============================================================================

TEST(ObjectAtomCoverage, CoffLocalFunctionIsItsOwnAtomWithInternalLinkage) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter wrep;
    auto obj = pe::encode(subjectModule(/*withLocalHelper=*/true), *loaded.target,
                          *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u) << "the WRITER accepts this module -- the "
                                        "object is well formed; the reader was "
                                        "the half that could not represent it";
    ASSERT_FALSE(obj.empty());

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value())
        << "a file-local function is a whole body and must reconstruct; errors="
        << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(sawCode(rep, DiagnosticCode::F_ObjectReaderSymbolBodyDropped))
        << "nothing is uncovered any more -- the guard must be SILENT here, not "
           "merely non-fatal";

    // TWO atoms, not one grown one. The count is asserted separately from the
    // byte total because the total alone cannot distinguish "helper became its
    // own function" from "helper's bytes were absorbed into entry".
    EXPECT_EQ(got->functions.size(), 2u)
        << "the local helper and the external entry are separate atoms";
    EXPECT_EQ(reconstructedTextBytes(*got), kHelperLen + kEntryLen);

    // DSS renames an internal-linkage function to `sym_<id>`, so the object's
    // spelling is the one to look up (the same spelling the refusal used to
    // print, for the same reason).
    std::string const localName = dssLocalSpelling(SymbolId{1});
    auto const* helper = funcNamed(*got, localName);
    ASSERT_NE(helper, nullptr)
        << "the file-local function must reconstruct as its own "
           "AssembledFunction under its object spelling " << localName;
    EXPECT_EQ(helper->bytes.size(), kHelperLen)
        << "...carrying its OWN extent, not the whole section and not the "
           "remainder after `entry`";

    auto const* helperSym = symbolNamed(*got, localName);
    ASSERT_NE(helperSym, nullptr);
    EXPECT_EQ(helperSym->binding, SymbolBinding::Local)
        << "internal linkage must survive the round trip: `resolveCrossCuDefs` "
           "SKIPS Local, which is what stops this definition from satisfying "
           "another translation unit's extern. Global would collide across "
           "members; Weak would let one member's private body be silently "
           "dropped as a shadowed duplicate";

    auto const* entrySym = symbolNamed(*got, "entry");
    ASSERT_NE(entrySym, nullptr);
    EXPECT_EQ(entrySym->binding, SymbolBinding::Global)
        << "and the EXTERNAL symbol is unchanged -- the new arm must not have "
           "reclassified the linkage it already got right";
}

// ============================================================================
// PIN 6 -- MACH-O: THE HEADER FLAG IS THE WHOLE DIFFERENCE, AND THE REFUSAL IS
// STILL LIVE ON THE OTHER SIDE OF IT.
//
// ⓘ THIS PIN USED TO ASSERT ONLY THE REFUSAL, on an object DSS itself wrote.
// That object now declares `MH_SUBSECTIONS_VIA_SYMBOLS`, so it reconstructs --
// and trimming the pin to match would have deleted the only place the shared
// refusal is exercised through a real reader. Instead both sides are pinned from
// ONE encode: the object as written, and the SAME BYTES with the header flag
// cleared. Nothing else differs, so whatever the two arms disagree about is that
// flag and nothing else.
//
// ★ WHY THE FLAG-LESS ARM IS NOT A STRAW MAN. A `.o` produced by an assembler or
// an older toolchain that never sets the bit is exactly this shape.
//
// ⓘ AND ITS ANSWER CHANGED MID-CYCLE, by operator ruling: the geometry fallback
// now applies to all three readers, so a flag-less local that no atom covers is
// RECOVERED rather than refused. The pin is stronger for it -- instead of
// "declared reconstructs, undeclared refuses" it now asserts that the two routes
// produce the SAME atoms. The refusal itself did not become untestable: it moved
// to the shape geometry is forbidden to touch (an N_ALT_ENTRY symbol with no
// atom before it), pinned as `AltEntryWithNoAtomBeforeItIsRefusedNotRecovered`
// in `test_macho_object_reader.cpp`.
// ============================================================================

TEST(ObjectAtomCoverage, MachoSubsectionsFlagAndGeometryAgreeAtomForAtom) {
    auto loaded = loadShipped("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter wrep;
    auto obj = macho::encode(subjectModule(/*withLocalHelper=*/true), *loaded.target,
                             *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_FALSE(obj.empty());

    // -- (a) AS WRITTEN: the object declares subsections, so the file-local
    //        function is an atom of its own.
    DiagnosticReporter repA;
    auto gotA = macho::readRelocatableObject(obj, *loaded.target, *loaded.format, repA);
    ASSERT_TRUE(gotA.has_value())
        << "a DSS mach-o object declares MH_SUBSECTIONS_VIA_SYMBOLS, so a "
           "file-local body must reconstruct; errors=" << repA.errorCount();
    EXPECT_FALSE(sawCode(repA, DiagnosticCode::F_ObjectReaderSymbolBodyDropped));
    EXPECT_EQ(gotA->functions.size(), 2u)
        << "two atoms, not one grown one";
    EXPECT_EQ(reconstructedTextBytes(*gotA), kHelperLen + kEntryLen);
    auto const* helperSym = symbolNamed(*gotA, "_" + dssLocalSpelling(SymbolId{1}));
    ASSERT_NE(helperSym, nullptr)
        << "mach-o C mangling prepends `_`, so the object's spelling of the "
           "synthesized internal-linkage name is `_sym_1`";
    EXPECT_EQ(helperSym->binding, SymbolBinding::Local)
        << "internal linkage survives the round trip";

    // -- (b) THE SAME BYTES with MH_SUBSECTIONS_VIA_SYMBOLS cleared. --
    // mach_header_64 is magic/cputype/cpusubtype/filetype/ncmds/sizeofcmds/
    // flags/reserved, all u32 -- so `flags` is the SEVENTH word. Derived from
    // that layout rather than written as a literal offset, because a literal is
    // the kind of number that silently stops pointing at the field it names.
    constexpr std::size_t kMachHeaderFlagsOff = 6u * sizeof(std::uint32_t);
    ASSERT_GT(obj.size(), kMachHeaderFlagsOff + 4u);
    constexpr std::uint32_t kMhSubsectionsViaSymbols = 0x2000u;
    std::uint32_t const flags = readU32LE(obj, kMachHeaderFlagsOff);
    ASSERT_NE(flags & kMhSubsectionsViaSymbols, 0u)
        << "the (a) arm above is only meaningful if the flag is actually SET in "
           "the bytes -- if this fires, (a) proved nothing";
    auto flagless = obj;
    std::uint32_t const cleared = flags & ~kMhSubsectionsViaSymbols;
    for (int i = 0; i < 4; ++i) {
        flagless[kMachHeaderFlagsOff + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((cleared >> (i * 8)) & 0xFFu);
    }

    DiagnosticReporter repB;
    auto gotB = macho::readRelocatableObject(flagless, *loaded.target,
                                             *loaded.format, repB);
    ASSERT_TRUE(gotB.has_value())
        << "with no subsection declaration the wire says nothing, so GEOMETRY "
           "decides -- and offset 0 is covered by no atom, so it starts a body; "
           "errors=" << repB.errorCount();
    EXPECT_EQ(repB.errorCount(), 0u);

    // ★ THE TWO ROUTES MUST AGREE, ATOM FOR ATOM. That is the real content of
    // this pin now: a DECLARATION and an INFERENCE reaching the same
    // reconstruction is what makes the fallback safe to apply to objects nobody
    // declared anything about. A count-and-extent comparison says it; a byte
    // total would not, since both worlds hold 12 bytes.
    ASSERT_EQ(gotB->functions.size(), gotA->functions.size());
    EXPECT_EQ(reconstructedTextBytes(*gotB), reconstructedTextBytes(*gotA));
    auto const* flaglessHelper =
        funcNamed(*gotB, "_" + dssLocalSpelling(SymbolId{1}));
    ASSERT_NE(flaglessHelper, nullptr)
        << "the file-local body must reconstruct under its object spelling";
    EXPECT_EQ(flaglessHelper->bytes.size(), kHelperLen)
        << "...with its OWN extent, identical to the declared route's";
}

// ============================================================================
// PIN 2 -- THE SIZE PIN. A reader that returns a module must have reconstructed
// the WHOLE text section.
//
// ★ THIS IS THE ASSERTION THAT WOULD HAVE CAUGHT THE DEFECT. Before the guard,
// the COFF/Mach-O `withLocalHelper` read returned a module whose function bytes
// summed to kEntryLen while the section held kEntryLen + kHelperLen -- green,
// and short by exactly the dropped function. `EXPECT_TRUE(read.has_value())`
// could not see that; this can.
//
// ⚠ A SUM IS NOT ENOUGH BY ITSELF, so each arm also asserts the ATOM COUNT.
// Bytes can be conserved while atoms are wrong in both directions: absorbing a
// missed boundary into the previous atom keeps the total, and splitting a
// function at an interior label keeps it too.
// ============================================================================

TEST(ObjectAtomCoverage, EveryReaderReconstructsTheWholeTextSection) {
    struct Leg {
        char const* targetName;
        char const* formatName;
        EncodeFn                     encode;
        ReadFn                       read;
        std::optional<std::uint64_t> (*textSize)(std::vector<std::uint8_t> const&);
    };
    // ⓘ THE LEG TABLE USED TO CARRY A `localFunctionIsAnAtom` FLAG, and this
    // test used to branch on it: two legs asserted reconstruction and the
    // Mach-O leg asserted the refusal, because `macho.cpp` wrote a local
    // function and a block label as the same bare N_SECT and nothing could tell
    // them apart. All three now have per-format evidence -- ELF `st_size`, COFF
    // IMAGE_SYM_DTYPE_FUNCTION, Mach-O MH_SUBSECTIONS_VIA_SYMBOLS +
    // N_ALT_ENTRY -- so the flag would be `true` in every row, and a branch
    // nothing takes is a branch nothing tests. The refusal half moved to the
    // object that still genuinely has no evidence: PIN 6's flag-less Mach-O.
    Leg const legs[] = {
        {"x86_64", "elf64-x86_64-linux",   &encodeElf,   &elf::readRelocatableObject,
         &elfTextSize},
        {"x86_64", "pe64-x86_64-windows",  &encodePe,    &pe::readRelocatableObject,
         &coffTextSize},
        {"arm64",  "macho64-arm64-darwin", &encodeMacho, &macho::readRelocatableObject,
         &machoTextSize},
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
        EXPECT_EQ(gotA->functions.size(), 1u)
            << "one source function, one atom -- the baseline the (b) count is "
               "measured against";

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
        // Every reader has per-format evidence now, so every reader must
        // reconstruct the grown section WHOLE -- pinned for all three so none
        // can regress back into the demotion it just left.
        ASSERT_TRUE(gotB.has_value())
            << "a short reconstruction must never be returned green -- it would "
               "have summed to " << *sizeA << " against a section of " << *sizeB
            << "; errors=" << repB.errorCount();
        EXPECT_EQ(repB.errorCount(), 0u);
        EXPECT_EQ(reconstructedTextBytes(*gotB), *sizeB);
        EXPECT_EQ(reconstructedTextBytes(*gotB),
                  reconstructedTextBytes(*gotA) + kHelperLen)
            << "the reconstruction GROWS with the source -- the exact property "
               "whose absence was the silent miscompile";
        // ★ AND IT GREW BY AN ATOM, not by a longer one. The byte total alone
        // cannot tell "helper reconstructed" from "helper's bytes were absorbed
        // into entry's extent", and absorption is a real failure mode here:
        // atoms end at the NEXT boundary, so a symbol left out of the boundary
        // set silently widens its predecessor.
        EXPECT_EQ(gotB->functions.size(), gotA->functions.size() + 1u)
            << "the file-local function must be its OWN atom";
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
        // ⚠⚠ THE COUNT IS NOT REDUNDANT WITH THE SUM, and this pin was a SILENT
        // PASS without it. `reconstructedTextBytes` SUMS: if the label were
        // wrongly promoted to an atom boundary, `entry` would split into 8 + 8
        // and the sum would still be 16 -- green under a miscompile that had
        // torn a function in half at an interior label, with every relocation
        // beyond byte 8 rerouted to the wrong item. On COFF that is now one
        // dropped mask away (`declaresFunction` returning true for a type-0
        // symbol promotes every block label), so the assertion that discriminates
        // is HOW MANY atoms came back, not how many bytes.
        EXPECT_EQ(got->functions.size(), 1u)
            << "an interior label must not SPLIT its enclosing function -- one "
               "source function, one atom";
    }
}

// ============================================================================
// PIN 1b -- A FOREIGN-SHAPED OBJECT, where a file-local function keeps its
// SOURCE name.
//
// PIN 1 uses an object DSS wrote, where every internal-linkage name is the
// synthesized `sym_<id>`. This uses a hand-rolled `.obj` whose STATIC symbol is
// literally named `helper` -- the shape a real cl.exe / mingw-gcc object has --
// so the reader is proven to be reading the OBJECT rather than recognising DSS's
// own naming pattern.
//
// ⓘ Names must be <= 8 bytes: this builder emits the INLINE COFF name form, and
// a longer one is silently TRUNCATED into the 8-byte field rather than spilling
// to the string table.
// ============================================================================

TEST(ObjectAtomCoverage, ForeignShapedCoffLocalFunctionWithTypeHintIsAnAtom) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    // `.text` = 12 bytes: a STATIC `helper` DECLARING DTYPE_FUNCTION at [0, 8)
    // and an EXTERNAL `entry` at 8. ✔MEASURED that this is exactly what real
    // producers emit for `static int helper(int);`: mingw gcc (Strawberry)
    // `(ty 20)(scl 3)` and MSVC cl.exe 14.51.36231 `(ty 20)(scl 3)`.
    std::vector<std::uint8_t> body(12, 0xC3);
    auto const obj = buildOneSectionCoff(
        body, {BSym{"helper", 0u, kClassStatic, kDtypeFunction},
               BSym{"entry", 8u, kClassExternal, kDtypeFunction}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errors=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    ASSERT_EQ(got->functions.size(), 2u)
        << "both the file-local and the external function are atoms";
    auto const* helper = funcNamed(*got, "helper");
    ASSERT_NE(helper, nullptr)
        << "a foreign object's file-local function keeps its SOURCE name, and "
           "must reconstruct under it";
    EXPECT_EQ(helper->bytes.size(), 8u)
        << "sliced to the next boundary (`entry` at 8), not to the section end";
    auto const* helperSym = symbolNamed(*got, "helper");
    ASSERT_NE(helperSym, nullptr);
    EXPECT_EQ(helperSym->binding, SymbolBinding::Local)
        << "class STATIC is C internal linkage -- it must never satisfy another "
           "TU's extern";
    EXPECT_EQ(reconstructedTextBytes(*got), body.size());
}

// ★ THE TRAILING POSITION, which the coverage GUARD alone can never see -- and
// therefore its own test rather than a tail on the one above, whose leading
// ASSERT would short-circuit past it under exactly the regression this pins.
//
// When the demoted symbol comes LAST, the preceding atom runs to the end of the
// section, so its offset stays COVERED and the guard is SILENT while its bytes
// ride inside the wrong atom. That misattribution is the guard's stated blind
// spot (`object_atom_coverage.hpp`'s KNOWN BOUNDARY), and no refusal will ever
// report it. Being a BOUNDARY symbol is what removes the blind spot, so the two
// positions are pinned separately rather than assumed symmetric.
TEST(ObjectAtomCoverage, ForeignShapedCoffTrailingLocalFunctionIsNotAbsorbed) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> body(12, 0xC3);
    auto const trailing = buildOneSectionCoff(
        body, {BSym{"entry", 0u, kClassExternal, kDtypeFunction},
               BSym{"helper", 8u, kClassStatic, kDtypeFunction}});
    DiagnosticReporter trep;
    auto trailGot =
        pe::readRelocatableObject(trailing, *loaded.target, *loaded.format, trep);
    ASSERT_TRUE(trailGot.has_value()) << "errors=" << trep.errorCount();
    ASSERT_EQ(trailGot->functions.size(), 2u);
    auto const* trailHelper = funcNamed(*trailGot, "helper");
    ASSERT_NE(trailHelper, nullptr);
    EXPECT_EQ(trailHelper->bytes.size(), 4u)
        << "a TRAILING file-local function takes the section's tail as its own "
           "atom instead of being absorbed into `entry`";
    auto const* trailEntry = funcNamed(*trailGot, "entry");
    ASSERT_NE(trailEntry, nullptr);
    EXPECT_EQ(trailEntry->bytes.size(), 8u)
        << "...which is visible from the OTHER side too: `entry` must stop at "
           "the local's offset, not swallow the rest of the section";
}

// ============================================================================
// PIN 4 -- THE GEOMETRY FALLBACK. The shape with NO wire evidence at all is
// recovered, not refused.
//
// ⓘ THIS PIN USED TO ASSERT THE REFUSAL, and the operator ruled the refusal was
// the wrong answer (2026-08-20). A class-STATIC symbol with no derived-type hint
// is the same three numbers whether it is an interior block label or a whole
// body, and IMAGE_SYMBOL declares no size -- but when NO reconstructed atom
// covers it, one of those two readings is eliminated: a label lies inside its
// enclosing function, and nothing encloses this one. So the object is not
// undecidable after all, and refusing it threw away bytes the reader could have
// placed. See `uncoveredDefinedSymbolsThatStartAnAtom`.
//
// ★ THE DIRECTION IS THE ARGUMENT. Promoting keeps the bytes (they become their
// own atom); demoting drops them. Between "possibly the wrong atom" and "no atom
// at all", only one is a silent byte loss.
// ============================================================================

TEST(ObjectAtomCoverage, CoffStaticWithNoTypeHintIsRecoveredByGeometry) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    // The SAME object as PIN 1b with ONE field changed: `helper` declares no
    // derived type. That single u16 used to be the whole difference between
    // "reconstructed" and "refused"; it is now the difference between two
    // ROUTES to the same reconstruction -- the wire hint, and geometry.
    std::vector<std::uint8_t> body(12, 0xC3);
    auto const obj = buildOneSectionCoff(
        body, {BSym{"helper", 0u, kClassStatic, kDtypeNone},
               BSym{"entry", 8u, kClassExternal, kDtypeFunction}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value())
        << "[0, 8) is owned by no atom, so `helper` cannot be interior to one -- "
           "it must be recovered as a body, not refused; errors="
        << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(sawCode(rep, DiagnosticCode::F_ObjectReaderSymbolBodyDropped))
        << "the post-condition must be SILENT once the fallback has placed the "
           "symbol, not merely non-fatal";

    ASSERT_EQ(got->functions.size(), 2u)
        << "two atoms -- the count is what separates 'recovered' from 'absorbed "
           "into entry', which the byte total cannot";
    auto const* helper = funcNamed(*got, "helper");
    ASSERT_NE(helper, nullptr);
    EXPECT_EQ(helper->bytes.size(), 8u)
        << "sliced to the next boundary (`entry` at 8), exactly as the wire-hint "
           "route slices it";
    auto const* entry = funcNamed(*got, "entry");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->bytes.size(), 4u);
    EXPECT_EQ(reconstructedTextBytes(*got), body.size());

    auto const* helperSym = symbolNamed(*got, "helper");
    ASSERT_NE(helperSym, nullptr);
    EXPECT_EQ(helperSym->binding, SymbolBinding::Local)
        << "a recovered body is still internal linkage -- geometry says WHERE "
           "the bytes are, never who may see them";
    // ONE ModuleSymbol, not two. The symbol was recorded as bodiless BEFORE the
    // fallback promoted it, so a promotion that also pushed a fresh
    // ModuleSymbol would leave the name in `mod.symbols` twice, and the
    // cross-CU resolve would see a duplicate definition of a Local.
    std::size_t named = 0;
    for (auto const& s : got->symbols) named += (s.name == "helper") ? 1u : 0u;
    EXPECT_EQ(named, 1u) << "a promoted symbol must not be recorded twice";
}

// ★★ THE SOUNDNESS BOUNDARY, and the reason the fallback is not simply
// "promote everything uncovered".
//
// The inference is a contrapositive: an interior label lies inside its enclosing
// function, so an uncovered symbol is not interior TO A RECONSTRUCTED ATOM. It
// says nothing about being interior to a body that is ALSO still unreconstructed
// -- and `{helper@0, L@4}` before an atom at 8 is exactly that: either two
// bodies, or one body at 0 with a label at 4, with nothing on the wire to tell
// them apart. Promoting both would SPLIT a function at an interior label, which
// the linker may then lay out with padding between the halves -- a silent
// miscompile. Promoting only the run's first merges them instead, which is a
// misattribution with every byte preserved.
//
// ⚠ THE BYTE TOTAL IS 12 IN BOTH WORLDS, so the assertion that discriminates is
// the atom COUNT and the first atom's EXTENT.
// RED-ON-DISABLE: delete the run check in
// `uncoveredDefinedSymbolsThatStartAnAtom` -> three atoms of 4 + 4 + 4.
TEST(ObjectAtomCoverage, SecondUncoveredSymbolInARunDoesNotSplitTheBody) {
    auto loaded = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<std::uint8_t> body(12, 0xC3);
    auto const obj = buildOneSectionCoff(
        body, {BSym{"helper", 0u, kClassStatic, kDtypeNone},
               BSym{"L", 4u, kClassStatic, kDtypeNone},
               BSym{"entry", 8u, kClassExternal, kDtypeFunction}});

    DiagnosticReporter rep;
    auto got = pe::readRelocatableObject(obj, *loaded.target, *loaded.format, rep);
    ASSERT_TRUE(got.has_value()) << "errors=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    ASSERT_EQ(got->functions.size(), 2u)
        << "the SECOND uncovered symbol of a run is undecidable, and the safe "
           "reading merges it into the first rather than splitting a body";
    auto const* helper = funcNamed(*got, "helper");
    ASSERT_NE(helper, nullptr);
    EXPECT_EQ(helper->bytes.size(), 8u)
        << "`helper` must run to the next REAL boundary (`entry` at 8), "
           "swallowing `L` -- not stop at 4";
    EXPECT_EQ(funcNamed(*got, "L"), nullptr)
        << "`L` backs no atom of its own";
    EXPECT_NE(symbolNamed(*got, "L"), nullptr)
        << "...but it is still a ModuleSymbol, so a relocation naming it still "
           "resolves by identity";
    EXPECT_EQ(reconstructedTextBytes(*got), body.size());
}

// ============================================================================
// PIN 5 -- THE SHARED HEADER, EXERCISED DIRECTLY.
//
// ★ WHY THESE EXIST SEPARATELY FROM THE READER PINS. The COFF reader now
// promotes every staged symbol its atoms do not cover, so no COFF OBJECT can
// reach the post-condition's refusal any more -- and a post-condition nothing
// can reach is a guard that asserts nothing unless something else proves it
// live. These call both functions with the coordinates a reader would hand them,
// which is the only place the refusal's own logic is still executed, and the
// only place the fallback's rules are pinned independently of one reader's
// classifier.
// ============================================================================

TEST(ObjectAtomCoverage, SharedCoveragePostConditionStillRefusesAnUncoveredSymbol) {
    using dss::link::format::BodilessDefinedSymbol;
    using dss::link::format::ReconstructedAtomExtent;

    // The exact shape a mis-keyed promotion produces: an atom EXISTS, in the
    // wrong section. Nothing downstream would notice -- the bytes went to an
    // atom, just not this symbol's.
    std::vector<BodilessDefinedSymbol> const staged = {
        BodilessDefinedSymbol{/*sectionKey=*/1, /*sectionOffset=*/0,
                              /*declaredSize=*/std::nullopt, "orphan", ".text"},
    };
    std::vector<ReconstructedAtomExtent> const atoms = {
        ReconstructedAtomExtent{/*sectionKey=*/2, 0, 64},
    };

    DiagnosticReporter rep;
    EXPECT_FALSE(dss::link::format::everyDefinedSymbolIsCoveredByAnAtom(
        staged, atoms, "test::reader", rep));
    ASSERT_TRUE(sawCode(rep, DiagnosticCode::F_ObjectReaderSymbolBodyDropped));
    std::string const detail =
        detailFor(rep, DiagnosticCode::F_ObjectReaderSymbolBodyDropped);
    EXPECT_NE(detail.find("orphan"), std::string::npos) << detail;
    EXPECT_NE(detail.find(".text"), std::string::npos) << detail;
    EXPECT_NE(detail.find("test::reader"), std::string::npos)
        << "the caller's own prefix, so a triager knows WHICH reader; got: "
        << detail;

    // Same symbol, an atom that actually covers it -> silent.
    std::vector<ReconstructedAtomExtent> const covering = {
        ReconstructedAtomExtent{/*sectionKey=*/1, 0, 64},
    };
    DiagnosticReporter ok;
    EXPECT_TRUE(dss::link::format::everyDefinedSymbolIsCoveredByAnAtom(
        staged, covering, "test::reader", ok));
    EXPECT_EQ(ok.errorCount(), 0u);
}

TEST(ObjectAtomCoverage, SharedGeometryFallbackPromotesExactlyTheRunStarts) {
    using dss::link::format::BodilessDefinedSymbol;
    using dss::link::format::ReconstructedAtomExtent;
    using dss::link::format::uncoveredDefinedSymbolsThatStartAnAtom;

    auto sym = [](std::uint32_t sec, std::uint64_t off, char const* name,
                  std::optional<std::uint64_t> size = std::nullopt) {
        return BodilessDefinedSymbol{sec, off, size, name, ".text"};
    };

    // Section 1: an atom at [16, 32). Candidates at 0 and 4 form ONE uncovered
    // run (only 0 is promoted); 20 is covered; 40 is past the atom's end and
    // starts a SECOND run.
    // Section 2: no atom at all, so its lone candidate is its own run start --
    // and it must not be suppressed by section 1's run.
    std::vector<BodilessDefinedSymbol> const staged = {
        sym(1, 0, "a"), sym(1, 4, "a_label"), sym(1, 20, "interior"),
        sym(1, 40, "b"), sym(2, 8, "other_section"),
    };
    std::vector<ReconstructedAtomExtent> const atoms = {
        ReconstructedAtomExtent{1, 16, 16},
    };

    auto const promote = uncoveredDefinedSymbolsThatStartAnAtom(staged, atoms);
    ASSERT_EQ(promote.size(), 3u);
    EXPECT_EQ(promote[0], 0u) << "`a` starts the first run";
    EXPECT_EQ(promote[1], 3u) << "`b` starts a run after the atom";
    EXPECT_EQ(promote[2], 4u) << "a different section is a different run";
    // Indices are returned ASCENDING so a caller can walk them against its own
    // staging order without re-sorting.
    EXPECT_TRUE(std::is_sorted(promote.begin(), promote.end()));

    // A DECLARED-EMPTY symbol is a marker (an ARM `$d`, an empty object): there
    // are no bytes to recover, so it is never promoted even though nothing
    // covers it.
    std::vector<BodilessDefinedSymbol> const marker = {sym(3, 0, "$d", 0u)};
    EXPECT_TRUE(uncoveredDefinedSymbolsThatStartAnAtom(marker, {}).empty());

    // A DECLARED SIZE bounds the run instead of the next atom: `sized` is 4
    // bytes, so `after` at 8 is OUTSIDE it and starts its own body. This is the
    // ELF `st_size` case, and it is why the run rule reads the declared extent
    // rather than always reaching to the next atom.
    std::vector<BodilessDefinedSymbol> const sized = {
        sym(4, 0, "sized", 4u), sym(4, 8, "after", 4u),
    };
    auto const bothSized = uncoveredDefinedSymbolsThatStartAnAtom(sized, {});
    ASSERT_EQ(bothSized.size(), 2u)
        << "a stated extent is evidence; only an INFERRED one has to merge";
}

// ── THE EQUAL-OFFSET ALIAS RULE, asked of the shared rule directly ─────────
//
// D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS. The three readers pin
// their own wiring; these pin the DECISION, in the neutral coordinates, where a
// misranked tie or a mis-grouped section would otherwise only show up as one
// reader's odd atom count.

namespace {
[[nodiscard]] dss::link::format::AtomStartCandidate
cand(std::uint32_t sec, std::uint64_t off, std::uint32_t id, char const* name,
     SymbolBinding bind = SymbolBinding::Global,
     std::optional<std::uint64_t> extent = std::nullopt,
     SymbolVisibility vis = SymbolVisibility::Default) {
    return dss::link::format::AtomStartCandidate{sec,  off,  extent, id,
                                                 bind, vis,  name,   ".text"};
}
} // namespace

TEST(ObjectAtomCoverage, SharedAliasRuleGivesTheAtomToTheExternallyVisibleName) {
    using dss::link::format::resolveEqualOffsetAtomAliases;

    // clang's shape: a LOCAL section label at symbol index 0 sharing offset 0
    // with an EXTERNAL function at index 2, plus an unrelated atom at 24.
    std::vector<dss::link::format::AtomStartCandidate> const c = {
        cand(1, 0, 0, "ltmp0", SymbolBinding::Local),
        cand(1, 24, 1, "_entry"),
        cand(1, 0, 2, "_outer"),
    };
    std::vector<std::uint32_t> owner;
    DiagnosticReporter rep;
    ASSERT_TRUE(resolveEqualOffsetAtomAliases(c, owner, "test::reader", rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(owner.size(), 3u);
    EXPECT_EQ(owner[0], 2u)
        << "the local label must defer to the external name it labels -- an "
           "index-only tie-break would hand the body to `ltmp0`, whose id no "
           "foreign linker can resolve";
    EXPECT_EQ(owner[2], 2u) << "the winner owns itself";
    EXPECT_EQ(owner[1], 1u) << "a lone candidate is untouched";
}

TEST(ObjectAtomCoverage, SharedAliasRuleGivesTheAtomToTheStrongNameOverTheWeak) {
    using dss::link::format::resolveEqualOffsetAtomAliases;

    // gcc's weak-alias shape, with the WEAK name at the LOWER index so the
    // ranking (not the index) is what decides.
    std::vector<dss::link::format::AtomStartCandidate> const c = {
        cand(1, 0, 3, "f", SymbolBinding::Weak, 21u),
        cand(1, 0, 5, "g", SymbolBinding::Global, 21u),
    };
    std::vector<std::uint32_t> owner;
    DiagnosticReporter rep;
    ASSERT_TRUE(resolveEqualOffsetAtomAliases(c, owner, "test::reader", rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(owner[0], 5u) << "the strong definition owns the body";
    EXPECT_EQ(owner[1], 5u);
}

TEST(ObjectAtomCoverage, SharedAliasRuleBreaksARemainingTieByObjectOrder) {
    using dss::link::format::resolveEqualOffsetAtomAliases;

    // Two equally-ranked names: the answer must be TOTAL, or the reconstruction
    // depends on which of two equal elements an unstable sort happened to keep.
    std::vector<dss::link::format::AtomStartCandidate> const c = {
        cand(1, 0, 7, "b"), cand(1, 0, 4, "a"),
    };
    std::vector<std::uint32_t> owner;
    DiagnosticReporter rep;
    ASSERT_TRUE(resolveEqualOffsetAtomAliases(c, owner, "test::reader", rep));
    EXPECT_EQ(owner[0], 4u);
    EXPECT_EQ(owner[1], 4u);
}

TEST(ObjectAtomCoverage, SharedAliasRuleNeverGroupsAcrossSectionsOrOffsets) {
    using dss::link::format::resolveEqualOffsetAtomAliases;

    // Same OFFSET, different SECTION -- and a same-section pair one byte apart.
    // Neither is an alias set; grouping either would fold unrelated bodies.
    std::vector<dss::link::format::AtomStartCandidate> const c = {
        cand(1, 0, 1, "text_a"), cand(2, 0, 2, "data_a"),
        cand(1, 1, 3, "text_b"),
    };
    std::vector<std::uint32_t> owner;
    DiagnosticReporter rep;
    ASSERT_TRUE(resolveEqualOffsetAtomAliases(c, owner, "test::reader", rep));
    EXPECT_EQ(owner[0], 1u);
    EXPECT_EQ(owner[1], 2u);
    EXPECT_EQ(owner[2], 3u);
}

TEST(ObjectAtomCoverage, SharedAliasRuleRefusesEqualOffsetWithConflictingExtents) {
    using dss::link::format::resolveEqualOffsetAtomAliases;

    // Nested, not aliased: a relocation past 8 belongs unambiguously to `outer`
    // while one below 8 is claimed by both. No atom is right for both.
    std::vector<dss::link::format::AtomStartCandidate> const c = {
        cand(1, 0, 1, "outer", SymbolBinding::Global, 16u),
        cand(1, 0, 2, "inner", SymbolBinding::Global, 8u),
    };
    std::vector<std::uint32_t> owner;
    DiagnosticReporter rep;
    EXPECT_FALSE(resolveEqualOffsetAtomAliases(c, owner, "test::reader", rep));
    EXPECT_EQ(rep.errorCount(), 1u) << "at most one diagnostic, then stop";
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_CorruptedBinary));
    std::string const detail = detailFor(rep, DiagnosticCode::F_CorruptedBinary);
    EXPECT_NE(detail.find("outer"), std::string::npos);
    EXPECT_NE(detail.find("inner"), std::string::npos);
    EXPECT_NE(detail.find("DIFFERENT extents"), std::string::npos);

    // An UNKNOWN extent is not a conflicting one: COFF and Mach-O derive an
    // atom's end from the next boundary, so their candidates carry nullopt and
    // equal-offset ones necessarily get the same extent.
    std::vector<dss::link::format::AtomStartCandidate> const derived = {
        cand(1, 0, 1, "outer", SymbolBinding::Global, std::nullopt),
        cand(1, 0, 2, "inner", SymbolBinding::Global, std::nullopt),
    };
    DiagnosticReporter ok;
    EXPECT_TRUE(resolveEqualOffsetAtomAliases(derived, owner, "test::reader", ok));
    EXPECT_EQ(ok.errorCount(), 0u);
    EXPECT_EQ(owner[1], 1u);

    // ...and an UNKNOWN one sitting BETWEEN two conflicting known ones must not
    // hide them. No shipped reader mixes stated and derived extents in one
    // object -- which is precisely why a consecutive-pair comparison would have
    // looked correct forever while being wrong.
    std::vector<dss::link::format::AtomStartCandidate> const straddled = {
        cand(1, 0, 1, "outer", SymbolBinding::Global, 16u),
        cand(1, 0, 2, "middle", SymbolBinding::Global, std::nullopt),
        cand(1, 0, 3, "inner", SymbolBinding::Global, 8u),
    };
    DiagnosticReporter split;
    EXPECT_FALSE(resolveEqualOffsetAtomAliases(straddled, owner, "test::reader", split));
    EXPECT_EQ(split.errorCount(), 1u);
}
