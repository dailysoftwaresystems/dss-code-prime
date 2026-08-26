// `ObjectFormatSchema::looksLikeRelocatableObject()` — the raw-bytes probe the
// driver routes a compile input on.
//
// ★ THE DEFECT IT EXISTS FOR. A `.o` / `.obj` named as a compile input is
// handed to the tokenizer, which reports `illegal character 0x7f` — the first
// byte of the ELF magic, surfaced as if the author had typed it into a source
// file. Answering "these bytes are a relocatable object of my format" is what
// lets the driver route that file to the LINKER instead, and it must be
// answered by the format backends: a driver that decided it itself would be
// re-acquiring exactly the format identity `object_format_backend.hpp` exists
// to keep out of the substrate.
//
// ★★ WHAT MAKES THIS PREDICATE DIFFERENT FROM ITS FOUR SIBLINGS, AND WHY IT
// NEEDS A TEST OF ITS OWN. `isImageFlavor()` and friends read a schema that a
// loader has already validated. This one reads a FILE — arbitrary bytes, of
// arbitrary length, possibly truncated mid-header, possibly a near-miss of a
// format it is not. It is `noexcept` and it must not read past the end of the
// span, so the truncation cases below are not politeness: they are the
// contract, and an out-of-bounds read in a `noexcept` predicate is a crash in
// the driver before any diagnostic exists to explain it.
//
// ★★ THE THREE PROPERTIES EVERY CASE HERE IS ONE OF:
//
//   (1) DEFINITE, NOT HEURISTIC. The accepting patterns are built FROM THE
//       SHIPPED SCHEMA'S OWN DECLARED VALUES (`elf().fileClass`,
//       `pe().machine`) and from the enums whose enumerators ARE the wire
//       values (`ElfObjectType::Rel` IS ET_REL). A fixture that retyped `1`
//       and `0x8664` would keep passing on the day the config changed and the
//       probe stopped recognizing what this project actually emits.
//
//   (2) BYTE ORDER IS A PROPERTY OF THE FILE, NEVER OF THE HOST. DSS has a
//       live big-endian s390x leg, so a half-word read in host order is
//       correct everywhere anybody usually looks and inverted on the one leg
//       nobody is watching. Two cases below (`ElfBigEndianSchema…`,
//       `MachOFiletypeIsReadThroughTheMagicsOwnLens`) hand the probe a
//       DESYNCED fixture — a header declaring one encoding whose type word is
//       spelled in the other — and demand a refusal. On a little-endian host
//       those two are the only cases that can tell a correct lens from a
//       lucky one.
//
//   (3) A REJECTION IS ASSERTED BESIDE ITS ACCEPTANCE. Every negative case
//       here sits in a test that also asserts the corresponding positive, so
//       a probe that regressed to `return false` fails immediately instead of
//       going green with an ever-more-thorough list of things it refuses.
//
// ⚠ THE PE ARM IS THE ONE THAT CANNOT BE DECIDED BY A MAGIC. A COFF object has
// none — it begins at its `IMAGE_FILE_HEADER` — so "is this a COFF object" is
// undecidable in general, and the predicate answers the decidable question
// instead: "is this a COFF object FOR THIS FORMAT", keyed on the machine the
// schema declares. `CoffObjectWithADifferentMachineIsRejected` is therefore
// not a nicety; it is the pin on the thing that makes the PE answer definite
// at all.

#include "link/object_format_backend.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// ── The schemas under test, loaded from what we SHIP ────────────────────
//
// `loadShipped` rather than an inline fixture: the probe's whole claim is that
// it recognizes the objects THIS project's writers emit for THIS format, and a
// hand-copied identity block can drift from the shipped one without anybody
// noticing (the `test_object_format_backend_registry.cpp` precedent).
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const>
shipped(std::string_view name) {
    auto r = ObjectFormatSchema::loadShipped(name);
    if (!r) {
        ADD_FAILURE() << "cannot load shipped object format '" << name << "'";
        return nullptr;
    }
    return *r;
}

// ── Byte-pattern builders ───────────────────────────────────────────────
//
// Each builds ONLY the fields the probe is specified to read and leaves every
// other byte zero. That is deliberate and it is an assertion in itself: a
// probe that had started consulting `e_shoff`, `cputype` or
// `NumberOfSections` would see zeros here, so a fixture filled in "for
// realism" would hide the very drift these tests exist to catch.

void writeU16(std::vector<std::uint8_t>& b, std::size_t off,
              std::uint16_t v, bool bigEndian) {
    b[off + 0] = static_cast<std::uint8_t>(bigEndian ? (v >> 8) : (v & 0xFFu));
    b[off + 1] = static_cast<std::uint8_t>(bigEndian ? (v & 0xFFu) : (v >> 8));
}

void writeU32(std::vector<std::uint8_t>& b, std::size_t off,
              std::uint32_t v, bool bigEndian) {
    for (std::size_t i = 0; i < 4u; ++i) {
        std::size_t const shift = bigEndian ? (3u - i) : i;
        b[off + i] = static_cast<std::uint8_t>((v >> (shift * 8u)) & 0xFFu);
    }
}

// ELFDATA2LSB / ELFDATA2MSB. Spelled here as the psABI numbers because a TEST
// is allowed to state the expected wire value independently — that is the
// point of a test — but the ELF *type* values below are projected off
// `ElfObjectType` so the fixture cannot silently disagree with the loader
// about what "rel" means.
constexpr std::uint8_t kEiData2Lsb = 1;
constexpr std::uint8_t kEiData2Msb = 2;
constexpr std::uint8_t kEiClass32  = 1;

constexpr std::uint16_t kEtRel  = static_cast<std::uint16_t>(ElfObjectType::Rel);
constexpr std::uint16_t kEtExec = static_cast<std::uint16_t>(ElfObjectType::Exec);
constexpr std::uint16_t kEtDyn  = static_cast<std::uint16_t>(ElfObjectType::Dyn);

constexpr std::uint32_t kMhObject  = static_cast<std::uint32_t>(MachOObjectType::Object);
constexpr std::uint32_t kMhExecute = static_cast<std::uint32_t>(MachOObjectType::Execute);

constexpr std::uint32_t kMachOMagic64 = 0xFEEDFACFu;  // MH_MAGIC_64
constexpr std::uint32_t kMachOMagic32 = 0xFEEDFACEu;  // MH_MAGIC
constexpr std::uint32_t kFatMagic     = 0xCAFEBABEu;  // FAT_MAGIC
constexpr std::uint32_t kFatMagic64   = 0xCAFEBABFu;  // FAT_MAGIC_64

// `Elf64_Ehdr`-sized buffer. `eTypeBigEndian` is separate from `eiData` ON
// PURPOSE: passing them differently builds the DESYNCED fixture that catches a
// host-order read.
[[nodiscard]] std::vector<std::uint8_t>
elfPattern(std::uint8_t eiClass, std::uint8_t eiData, std::uint16_t eType,
           bool eTypeBigEndian) {
    std::vector<std::uint8_t> b(64, 0u);
    b[0] = 0x7Fu; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = eiClass;
    b[5] = eiData;
    writeU16(b, 16, eType, eTypeBigEndian);
    return b;
}

// The honest producer: `e_type` spelled the way the header's own EI_DATA says
// this file is spelled.
[[nodiscard]] std::vector<std::uint8_t>
elfPattern(std::uint8_t eiClass, std::uint8_t eiData, std::uint16_t eType) {
    return elfPattern(eiClass, eiData, eType, eiData == kEiData2Msb);
}

// `mach_header_64`-sized buffer. `filetypeBigEndian` is separate from
// `magicBigEndian` for the same reason `eTypeBigEndian` is separate above.
[[nodiscard]] std::vector<std::uint8_t>
machoPattern(std::uint32_t magic, bool magicBigEndian,
             std::uint32_t filetype, bool filetypeBigEndian) {
    std::vector<std::uint8_t> b(32, 0u);
    writeU32(b, 0, magic, magicBigEndian);
    writeU32(b, 12, filetype, filetypeBigEndian);
    return b;
}

[[nodiscard]] std::vector<std::uint8_t>
machoPattern(std::uint32_t magic, std::uint32_t filetype) {
    return machoPattern(magic, /*magicBigEndian=*/false, filetype,
                        /*filetypeBigEndian=*/false);
}

// A universal ("fat") header. `fat_header` is BIG-ENDIAN ON DISK by
// definition, whatever the slices inside it are — writing it little-endian
// would build a file no producer emits and prove nothing.
[[nodiscard]] std::vector<std::uint8_t> fatPattern(std::uint32_t fatMagic) {
    std::vector<std::uint8_t> b(32, 0u);
    writeU32(b, 0, fatMagic, /*bigEndian=*/true);
    writeU32(b, 4, 1u, /*bigEndian=*/true);   // nfat_arch = 1
    return b;
}

// `IMAGE_FILE_HEADER`-sized buffer: no DOS stub, no `PE\0\0` signature — a
// COFF object begins here, which is exactly why it has no magic to match.
[[nodiscard]] std::vector<std::uint8_t>
coffPattern(std::uint16_t machine, std::uint16_t sizeOfOptionalHeader) {
    std::vector<std::uint8_t> b(20, 0u);
    writeU16(b, 0, machine, /*bigEndian=*/false);
    writeU16(b, 16, sizeOfOptionalHeader, /*bigEndian=*/false);
    return b;
}

// A PE IMAGE: `MZ` DOS stub, `e_lfanew` pointing at a `PE\0\0` signature.
[[nodiscard]] std::vector<std::uint8_t> mzImagePattern() {
    std::vector<std::uint8_t> b(128, 0u);
    b[0] = 'M'; b[1] = 'Z';
    writeU16(b, 60, 64u, /*bigEndian=*/false);          // e_lfanew
    b[64] = 'P'; b[65] = 'E'; b[66] = 0u; b[67] = 0u;   // PE signature
    return b;
}

[[nodiscard]] std::span<std::uint8_t const>
view(std::vector<std::uint8_t> const& b) {
    return std::span<std::uint8_t const>{b};
}

// An in-memory schema through the VALIDATION-BYPASSING constructor — the path
// that exists (it is public, and `test_linker_diagnostic_vocabulary.cpp` uses
// it for the same reason) and the only way to reach identities no shipped
// `.format.json` declares: a big-endian ELF, and a PE schema whose declared
// machine is the one value that would let a DOS stub through.
[[nodiscard]] dss::detail::ObjectFormatData
handBuilt(char const* configName, char const* label) {
    dss::detail::ObjectFormatData data;
    data.name    = label;
    data.version = "0.1";
    data.backend = dss::link::objectFormatBackendByConfigName(configName);
    return data;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// ELF
// ─────────────────────────────────────────────────────────────────────────

TEST(RelocatableObjectProbe, ElfRelocatableAcceptedAndEveryOtherETypeRejected) {
    auto const fmt = shipped("elf64-x86_64-linux");
    ASSERT_NE(fmt, nullptr);

    std::uint8_t const cls  = fmt->elf().fileClass;
    std::uint8_t const data = fmt->elf().dataEncoding;
    // Anti-vacuity: a schema declaring neither would make every case below
    // pass for the wrong reason (no real ELF carries 0 in either byte).
    ASSERT_NE(cls, 0u) << "the shipped format declares no `elf.class`";
    ASSERT_NE(data, 0u) << "the shipped format declares no `elf.data`";

    auto const rel = elfPattern(cls, data, kEtRel);
    EXPECT_TRUE(fmt->looksLikeRelocatableObject(view(rel)))
        << "an ET_REL header spelled exactly as this format declares itself "
           "must be recognized — this is the whole routing decision";

    // ET_EXEC and ET_DYN are link OUTPUTS, not inputs. Recognizing one would
    // route an executable into the linker's object reader, which refuses it
    // by name (`elf::readRelocatableObject`) after the driver has already
    // told the operator it understood the file.
    auto const exec = elfPattern(cls, data, kEtExec);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(exec)))
        << "ET_EXEC accepted as a relocatable object";
    auto const dyn = elfPattern(cls, data, kEtDyn);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(dyn)))
        << "ET_DYN accepted as a relocatable object";
}

TEST(RelocatableObjectProbe, ElfWrongClassOrWrongEncodingIsRejected) {
    auto const fmt = shipped("elf64-x86_64-linux");
    ASSERT_NE(fmt, nullptr);

    std::uint8_t const cls  = fmt->elf().fileClass;
    std::uint8_t const data = fmt->elf().dataEncoding;
    ASSERT_NE(cls, kEiClass32)
        << "this pin assumes a 64-bit schema; an ELFCLASS32 format would need "
           "its own fixture";
    ASSERT_EQ(data, kEiData2Lsb)
        << "this pin assumes a little-endian schema";

    // The control: same bytes, right class + right encoding.
    auto const good = elfPattern(cls, data, kEtRel);
    ASSERT_TRUE(fmt->looksLikeRelocatableObject(view(good)));

    // An ELF32 ET_REL. Real file, real magic, real e_type — and a file this
    // format could not have written and its reader refuses by name
    // (`not ELFCLASS64`). Accepting it routes the operator to a refusal from
    // a component the driver claimed understood the file.
    auto const elf32 = elfPattern(kEiClass32, data, kEtRel);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(elf32)))
        << "an ELFCLASS32 object was accepted by a 64-bit schema";

    // A big-endian ET_REL, self-consistently spelled.
    auto const bigEndian = elfPattern(cls, kEiData2Msb, kEtRel);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(bigEndian)))
        << "a big-endian object was accepted by a little-endian schema";

    // EI_DATA outside the two defined encodings is not a header this probe
    // decodes at all — there is no third lens to read `e_type` through.
    auto const bogusEncoding = elfPattern(cls, 7u, kEtRel, /*eTypeBigEndian=*/false);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(bogusEncoding)))
        << "EI_DATA=7 (no such encoding) was accepted";
}

// ★★ THE s390x PIN. On a little-endian host every other ELF case here passes
// whether the probe reads `e_type` through the file's own EI_DATA or through
// the host's order. This one does not: the schema declares ELFDATA2MSB, so the
// probe must read `e_type` BIG-endian, and the two fixtures differ only in
// which order their `e_type` bytes are spelled.
//
// A host-order read gets both cases exactly backwards — accepting the file
// whose `01 00` means ET_NONE-with-a-stray-byte (0x0100 = 256) and rejecting
// the honest big-endian ET_REL. There is no shipped big-endian `.format.json`
// to load, so the schema comes through the validation-bypassing constructor.
TEST(RelocatableObjectProbe, ElfBigEndianSchemaReadsETypeThroughTheFilesOwnEncoding) {
    auto data = handBuilt("elf", "synth-elf-msb-probe-pin");
    ASSERT_NE(data.backend, nullptr) << "the 'elf' backend did not resolve";
    data.elf.fileClass    = 2u;              // ELFCLASS64
    data.elf.dataEncoding = kEiData2Msb;     // ELFDATA2MSB
    ObjectFormatSchema const fmt{data};

    auto const honest = elfPattern(2u, kEiData2Msb, kEtRel, /*eTypeBigEndian=*/true);
    EXPECT_TRUE(fmt.looksLikeRelocatableObject(view(honest)))
        << "a big-endian ET_REL, spelled big-endian, was not recognized by a "
           "big-endian schema — the probe is reading `e_type` in host order";

    auto const desynced = elfPattern(2u, kEiData2Msb, kEtRel, /*eTypeBigEndian=*/false);
    EXPECT_FALSE(fmt.looksLikeRelocatableObject(view(desynced)))
        << "a big-endian header whose `e_type` is spelled little-endian was "
           "accepted — the probe is reading `e_type` in host order";
}

// ⓘ Fail-closed on an identity nobody declared. The hand-built path leaves
// `fileClass`/`dataEncoding` at 0, and no ELF file carries 0 in either byte,
// so such a schema recognizes nothing — no extra arm in the probe, but the
// property is worth pinning because it is what keeps a half-initialized
// struct from matching a real object.
TEST(RelocatableObjectProbe, ElfSchemaDeclaringNoClassRecognizesNothing) {
    auto const data = handBuilt("elf", "synth-elf-undeclared-identity");
    ASSERT_NE(data.backend, nullptr);
    ObjectFormatSchema const fmt{data};

    auto const rel = elfPattern(2u, kEiData2Lsb, kEtRel);
    EXPECT_FALSE(fmt.looksLikeRelocatableObject(view(rel)));
}

// ─────────────────────────────────────────────────────────────────────────
// Mach-O
// ─────────────────────────────────────────────────────────────────────────

TEST(RelocatableObjectProbe, MachOObjectAcceptedAndImageFiletypesRejected) {
    auto const fmt = shipped("macho64-x86_64-darwin");
    ASSERT_NE(fmt, nullptr);

    auto const object = machoPattern(kMachOMagic64, kMhObject);
    EXPECT_TRUE(fmt->looksLikeRelocatableObject(view(object)))
        << "an MH_OBJECT header was not recognized";

    auto const execute = machoPattern(kMachOMagic64, kMhExecute);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(execute)))
        << "MH_EXECUTE accepted as a relocatable object";

    // Not a Mach-O magic at all: `0xFEEDFACD` is one bit off MH_MAGIC_64.
    auto const nearMiss = machoPattern(0xFEEDFACDu, kMhObject);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(nearMiss)))
        << "a near-miss magic was accepted";
}

// ★ A UNIVERSAL FILE IS NOT A RELOCATABLE OBJECT, even when it wraps one.
// Extracting a slice is `lipo`'s job; answering true here hands the linker a
// `fat_header` to parse as a `mach_header`. Both fat magics are pinned because
// `FAT_MAGIC_64` is what `lipo` emits once a slice crosses 4 GiB, and a probe
// that matched only the first would let the second through.
//
// ⚠ AND THE FIXTURE IS BIG-ENDIAN ON DISK BECAUSE THE FORMAT SAYS SO. Reading
// a fat magic through a little-endian lens yields `0xBEBAFECA` and matches
// nothing — a mistake this tree has already made once and documented at
// `ffi/binary_readers/reader_common.hpp`'s `kMachOFatMagic` block. Writing the
// fixture the same wrong way would make this test pass against a probe that
// accepted real universal files.
TEST(RelocatableObjectProbe, MachOUniversalFatHeaderIsRejected) {
    auto const fmt = shipped("macho64-x86_64-darwin");
    ASSERT_NE(fmt, nullptr);

    auto const control = machoPattern(kMachOMagic64, kMhObject);
    ASSERT_TRUE(fmt->looksLikeRelocatableObject(view(control)));

    auto const fat = fatPattern(kFatMagic);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(fat)))
        << "a FAT_MAGIC universal header was accepted as a relocatable object";
    auto const fat64 = fatPattern(kFatMagic64);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(fat64)))
        << "a FAT_MAGIC_64 universal header was accepted as a relocatable "
           "object";
}

// ★★ THE MACH-O HALF OF THE s390x PIN. A thin header stores its magic in the
// SLICE's own order, so a big-endian object opens `FE ED FA CF` — and its
// `filetype` must then be read big-endian too. The desynced fixture (big-endian
// magic, little-endian `filetype`) is the one a host-order read accepts.
TEST(RelocatableObjectProbe, MachOFiletypeIsReadThroughTheMagicsOwnLens) {
    auto const fmt = shipped("macho64-x86_64-darwin");
    ASSERT_NE(fmt, nullptr);

    auto const honest = machoPattern(kMachOMagic64, /*magicBigEndian=*/true,
                                     kMhObject, /*filetypeBigEndian=*/true);
    EXPECT_TRUE(fmt->looksLikeRelocatableObject(view(honest)))
        << "a big-endian thin Mach-O object was not recognized — the magic is "
           "the file's byte-order declaration and both lenses must be tried";

    auto const desynced = machoPattern(kMachOMagic64, /*magicBigEndian=*/true,
                                       kMhObject, /*filetypeBigEndian=*/false);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(desynced)))
        << "a big-endian header whose `filetype` is spelled little-endian was "
           "accepted — the probe is reading `filetype` in host order";
}

// ★★★ THE WIDTH RULING, 2026-08-26 (P36 lane L). THIS CASE USED TO ASSERT THE
// OPPOSITE, and the reversal is deliberate rather than a regression: the 32-bit
// thin magic was accepted by a `macho64` schema on the argument that "the magic
// already decides the family definitely". That argument belongs to the MACHINE
// axis, where all three backends agree to be loose. WIDTH is the SPELLING axis,
// which the ELF arm has always checked (`EI_CLASS` against the declared
// `fileClass`) for a reason that is just as true here: a 32-bit `mach_header`
// has no trailing `reserved` word and its load commands spell `section` rather
// than `section_64`, so EVERY table offset in the file moves and no 64-bit
// reader can decode one. The predicate's own contract — "an object THIS FORMAT
// could itself have written" — makes the loose answer wrong outright.
//
// ⓘ WHAT DID NOT CHANGE, and it is the half Lane H's argument was actually
// about: a WRONG-ARCH object still classifies. `macho64-arm64` and
// `macho64-x86_64` are both 64-bit, so such a file still reaches the reader's
// cputype-naming refusal rather than being dropped to the dynamic arm and
// producing the misleading "no `.dynsym`" error. `EachFormatRecognizesOnlyIts-
// OwnFamily` below and the ELF arm's own `e_machine` note are the controls.
TEST(RelocatableObjectProbe, MachO32BitThinObjectIsRejectedByA64BitSchema) {
    auto const fmt = shipped("macho64-x86_64-darwin");
    ASSERT_NE(fmt, nullptr);

    auto const thin32 = machoPattern(kMachOMagic32, kMhObject);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(thin32)))
        << "a 32-bit MH_OBJECT was accepted by a macho64 schema — the probe "
           "answers 'an object this format could have written', and this "
           "format writes `mach_header_64` only";

    // Asserted BESIDE its rejection (property (3) in the file header): the
    // 64-bit object of the SAME schema must still be accepted, or a probe that
    // regressed to `return false` would pass the case above.
    auto const thin64 = machoPattern(kMachOMagic64, kMhObject);
    EXPECT_TRUE(fmt->looksLikeRelocatableObject(view(thin64)))
        << "the 64-bit object of this schema's own width must still be "
           "recognized";
}

// ⚠ FAIL-CLOSED ON A SCHEMA THAT DECLARED NO CPUTYPE. The ELF arm gets this
// property for free — a zero `fileClass` matches no real file — but a zero
// `macho.cputype` has CPU_ARCH_ABI64 CLEAR, so without an explicit guard
// "declared nothing" would silently read as "declares 32-bit" and a
// cputype-less schema would start recognizing 32-bit objects. The
// validation-bypassing `ObjectFormatSchema{ObjectFormatData}` constructor makes
// such a schema reachable, which is what turns this from decoration into a
// live arm — the same shape `PeImageIsRejectedOnItsDosStubWhenTheStubReadsAs-
// TheDeclaredMachine` pins for `IMAGE_FILE_MACHINE_UNKNOWN`.
TEST(RelocatableObjectProbe, MachOSchemaDeclaringNoCputypeRecognizesNothing) {
    auto const shippedFmt = shipped("macho64-x86_64-darwin");
    ASSERT_NE(shippedFmt, nullptr);
    // The control: the shipped document DOES recognize its own object, so a
    // refusal below is attributable to the cleared cputype and to nothing else.
    auto const thin64 = machoPattern(kMachOMagic64, kMhObject);
    ASSERT_TRUE(shippedFmt->looksLikeRelocatableObject(view(thin64)));

    auto const data = handBuilt("macho", "synth-macho-undeclared-cputype");
    ASSERT_NE(data.backend, nullptr);
    ObjectFormatSchema const noCputype{data};   // macho.cputype left at 0
    EXPECT_FALSE(noCputype.looksLikeRelocatableObject(view(thin64)));
    auto const thin32 = machoPattern(kMachOMagic32, kMhObject);
    EXPECT_FALSE(noCputype.looksLikeRelocatableObject(view(thin32)))
        << "a schema declaring no cputype recognized a 32-bit object — a "
           "cleared ABI64 bit must mean 'declared nothing', never 'declares "
           "32-bit'";
}

// ─────────────────────────────────────────────────────────────────────────
// PE / COFF
// ─────────────────────────────────────────────────────────────────────────

TEST(RelocatableObjectProbe, CoffObjectAcceptedAndPeImageShapesRejected) {
    auto const fmt = shipped("pe64-x86_64-windows");
    ASSERT_NE(fmt, nullptr);
    std::uint16_t const machine = fmt->pe().machine;
    ASSERT_NE(machine, 0u) << "the shipped format declares no `pe.machine`";

    auto const obj = coffPattern(machine, /*sizeOfOptionalHeader=*/0u);
    EXPECT_TRUE(fmt->looksLikeRelocatableObject(view(obj)))
        << "a COFF object header declaring this format's own machine was not "
           "recognized";

    // A non-zero `SizeOfOptionalHeader` means a PE IMAGE — a link OUTPUT.
    // 240 is `IMAGE_OPTIONAL_HEADER64`'s size; any non-zero value must fail.
    auto const image = coffPattern(machine, /*sizeOfOptionalHeader=*/240u);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(image)))
        << "a header with an optional header was accepted as an object";

    // A PE IMAGE with its DOS stub. ⓘ For a format declaring an ordinary
    // machine this is decided by the machine equality (a stub reads as
    // machine 0x5A4D); the stub arm ITSELF is pinned separately below, where
    // it is the only rule that can fire.
    auto const mz = mzImagePattern();
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(mz)))
        << "an `MZ` PE image was accepted as a relocatable object";
}

// ★★★ THE PIN ON WHAT MAKES THE PE ANSWER DEFINITE. A COFF object carries no
// magic, so the ONLY thing separating "a relocatable object for this format"
// from "twenty bytes that happen to look plausible" is the machine this schema
// declared. A probe that accepted any machine would answer true for a foreign
// object it cannot link and for plenty of files that are not COFF at all.
TEST(RelocatableObjectProbe, CoffObjectWithADifferentMachineIsRejected) {
    auto const fmt = shipped("pe64-x86_64-windows");
    ASSERT_NE(fmt, nullptr);
    std::uint16_t const machine = fmt->pe().machine;

    constexpr std::uint16_t kMachineArm64 = 0xAA64u;  // IMAGE_FILE_MACHINE_ARM64
    ASSERT_NE(machine, kMachineArm64)
        << "the fixture's 'other' machine equals the declared one — this test "
           "would assert nothing";

    auto const mine = coffPattern(machine, 0u);
    ASSERT_TRUE(fmt->looksLikeRelocatableObject(view(mine)));

    auto const foreign = coffPattern(kMachineArm64, 0u);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(foreign)))
        << "a COFF object for a different machine was accepted";

    // Twenty zero bytes: machine `IMAGE_FILE_MACHINE_UNKNOWN`, optional-header
    // size 0. The shape that would pass if "is this COFF" were answered
    // without the schema's declaration.
    std::vector<std::uint8_t> const zeros(20, 0u);
    EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(zeros)))
        << "a run of zero bytes was accepted as a COFF object";
}

// ★★ THE `MZ` ARM, EXERCISED RATHER THAN READ — and the case is stranger than
// it looks. `'M','Z'` occupies the same two bytes as `Machine`, so a DOS stub
// READS AS machine 0x5A4D. For every other declared machine the stub check is
// therefore SUBSUMED by the machine equality, and there is exactly one
// identity where the arm decides anything: a schema declaring 0x5A4D itself.
//
// ⚠ THAT SCHEMA IS REACHABLE FROM A CONFIG, not only from the in-memory
// constructor — `readIdentity` range-checks `pe.machine` to `[0, 0xFFFF]` and
// `validateIdentity` requires only non-zero, so `"machine": 23117` loads. For
// such a format, deleting the stub arm makes EVERY PE image on the system
// answer true: the fixture below satisfies every other condition (its machine
// half-word IS the declared one and its `SizeOfOptionalHeader` IS zero, both
// asserted directly so the claim is not taken on trust), which is what makes
// this a red-on-disable rather than a restatement.
//
// ⓘ There is deliberately NO "same machine without the stub" control here:
// those bytes do not exist. A COFF header declaring machine 0x5A4D IS the
// byte sequence `'M','Z'`. Constructing one was the first draft of this test,
// and it fails — the two fixtures are the same file.
TEST(RelocatableObjectProbe, PeImageIsRejectedOnItsDosStubWhenTheStubReadsAsTheDeclaredMachine) {
    constexpr std::uint16_t kMzAsMachine = 0x5A4Du;   // 'M' | ('Z' << 8)

    auto data = handBuilt("pe", "synth-pe-mz-machine-probe-pin");
    ASSERT_NE(data.backend, nullptr) << "the 'pe' backend did not resolve";
    data.pe.machine = kMzAsMachine;
    ObjectFormatSchema const fmt{data};

    auto const mz = mzImagePattern();
    ASSERT_GE(mz.size(), 18u);
    // Everything except the stub rule is satisfied by this image — asserted,
    // not assumed, so a fixture that drifted could not quietly turn this test
    // into a tautology.
    ASSERT_EQ(static_cast<std::uint16_t>(mz[0] | (mz[1] << 8)), kMzAsMachine)
        << "the DOS stub no longer reads as the declared machine";
    ASSERT_EQ(mz[16], 0u);
    ASSERT_EQ(mz[17], 0u);

    EXPECT_FALSE(fmt.looksLikeRelocatableObject(view(mz)))
        << "a PE image was accepted as a relocatable object because its DOS "
           "stub reads as the declared machine";
}

// ─────────────────────────────────────────────────────────────────────────
// The kinds that ship no relocatable-object reader
// ─────────────────────────────────────────────────────────────────────────

TEST(RelocatableObjectProbe, WasmAndSpirvRecognizeNothing) {
    for (auto const* name : {"wasm32-v1", "spirv-1.6"}) {
        auto const fmt = shipped(name);
        ASSERT_NE(fmt, nullptr) << name;

        // Every pattern the three native arms accept, plus a plausible
        // native-to-them one: no reader exists, so recognizing any of them
        // would route a file to a component that cannot read it.
        auto const elf   = elfPattern(2u, kEiData2Lsb, kEtRel);
        auto const macho = machoPattern(kMachOMagic64, kMhObject);
        auto const coff  = coffPattern(0x8664u, 0u);
        std::vector<std::uint8_t> const wasmMagic{0x00u, 'a', 's', 'm',
                                                  0x01u, 0x00u, 0x00u, 0x00u};
        EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(elf))) << name;
        EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(macho))) << name;
        EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(coff))) << name;
        EXPECT_FALSE(fmt->looksLikeRelocatableObject(view(wasmMagic))) << name;
        EXPECT_FALSE(fmt->looksLikeRelocatableObject({})) << name;
    }
}

// ─────────────────────────────────────────────────────────────────────────
// The cross-family matrix, the truncation contract, and the null backend
// ─────────────────────────────────────────────────────────────────────────

// Each probe recognizes ITS OWN family's object and no other's. Off-diagonal
// acceptance is the failure that would make the driver route a Mach-O object
// into the COFF reader — a refusal one tier further down, after the driver has
// already committed.
TEST(RelocatableObjectProbe, EachFormatRecognizesOnlyItsOwnFamily) {
    auto const elfFmt   = shipped("elf64-x86_64-linux");
    auto const machoFmt = shipped("macho64-x86_64-darwin");
    auto const peFmt    = shipped("pe64-x86_64-windows");
    ASSERT_NE(elfFmt, nullptr);
    ASSERT_NE(machoFmt, nullptr);
    ASSERT_NE(peFmt, nullptr);

    auto const elfObj =
        elfPattern(elfFmt->elf().fileClass, elfFmt->elf().dataEncoding, kEtRel);
    auto const machoObj = machoPattern(kMachOMagic64, kMhObject);
    auto const coffObj  = coffPattern(peFmt->pe().machine, 0u);

    EXPECT_TRUE(elfFmt->looksLikeRelocatableObject(view(elfObj)));
    EXPECT_FALSE(elfFmt->looksLikeRelocatableObject(view(machoObj)));
    EXPECT_FALSE(elfFmt->looksLikeRelocatableObject(view(coffObj)));

    EXPECT_FALSE(machoFmt->looksLikeRelocatableObject(view(elfObj)));
    EXPECT_TRUE(machoFmt->looksLikeRelocatableObject(view(machoObj)));
    EXPECT_FALSE(machoFmt->looksLikeRelocatableObject(view(coffObj)));

    EXPECT_FALSE(peFmt->looksLikeRelocatableObject(view(elfObj)));
    EXPECT_FALSE(peFmt->looksLikeRelocatableObject(view(machoObj)));
    EXPECT_TRUE(peFmt->looksLikeRelocatableObject(view(coffObj)));
}

// ★★ THE `noexcept` / BOUNDS CONTRACT. The probe is handed whatever the file
// contained, and a compile input can legitimately be empty or a few bytes
// long. Every prefix of an ACCEPTED pattern is walked — 0, 1, 2, 4 and 15
// bytes, which straddle every field boundary the three arms read (the ELF
// magic at 4, `filetype` at 12..16, `SizeOfOptionalHeader` at 16..18) — and
// each must answer false without reading past the end.
//
// ⚠ WHAT THIS CANNOT PROVE ON ITS OWN: an out-of-bounds read of a `vector`'s
// heap buffer usually returns a byte rather than trapping, so a green run here
// is necessary and not sufficient. It is written against a probe whose reads
// are bounds-checked BY CONSTRUCTION (an optional-returning helper per arm),
// so the assertion this makes is that the checks are WIRED, and a sanitizer
// build is what would make the memory claim itself.
TEST(RelocatableObjectProbe, TruncatedAndEmptySpansAreRefusedNeverRead) {
    auto const elfFmt   = shipped("elf64-x86_64-linux");
    auto const machoFmt = shipped("macho64-x86_64-darwin");
    auto const peFmt    = shipped("pe64-x86_64-windows");
    ASSERT_NE(elfFmt, nullptr);
    ASSERT_NE(machoFmt, nullptr);
    ASSERT_NE(peFmt, nullptr);

    struct Case {
        char const*                            label;
        ObjectFormatSchema const*              fmt;
        std::vector<std::uint8_t>              accepted;
    };
    std::vector<Case> const cases{
        {"elf", elfFmt.get(),
         elfPattern(elfFmt->elf().fileClass, elfFmt->elf().dataEncoding, kEtRel)},
        {"macho", machoFmt.get(), machoPattern(kMachOMagic64, kMhObject)},
        {"pe", peFmt.get(), coffPattern(peFmt->pe().machine, 0u)},
    };

    for (auto const& c : cases) {
        // Anti-vacuity: the FULL pattern must be accepted, or "every prefix is
        // rejected" is satisfied by a probe that rejects everything.
        ASSERT_TRUE(c.fmt->looksLikeRelocatableObject(view(c.accepted)))
            << c.label << ": the full pattern is not accepted";

        for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{2},
                              std::size_t{4}, std::size_t{15}}) {
            ASSERT_LE(n, c.accepted.size()) << c.label;
            std::vector<std::uint8_t> const prefix{c.accepted.begin(),
                                                   c.accepted.begin()
                                                       + static_cast<std::ptrdiff_t>(n)};
            EXPECT_FALSE(c.fmt->looksLikeRelocatableObject(view(prefix)))
                << c.label << ": a " << n << "-byte prefix was accepted";
        }
        // A default-constructed span — null data pointer, zero size. The shape
        // a caller passes for a file it could not read at all.
        EXPECT_FALSE(c.fmt->looksLikeRelocatableObject({}))
            << c.label << ": an empty span was accepted";
    }
}

// The wrapper's null-backend arm: a document that resolved to no backend
// recognizes nothing, so an unresolvable format cannot route a file to a
// linker it has no writer for. Same fail-closed shape as `isImageFlavor()`.
TEST(RelocatableObjectProbe, NullBackendRecognizesNothing) {
    dss::detail::ObjectFormatData data;
    data.name    = "synth-unresolvable-probe-pin";
    data.version = "0.1";
    ASSERT_EQ(data.backend, nullptr)
        << "this pin requires an unresolved backend";
    ObjectFormatSchema const fmt{data};

    auto const elf   = elfPattern(2u, kEiData2Lsb, kEtRel);
    auto const macho = machoPattern(kMachOMagic64, kMhObject);
    auto const coff  = coffPattern(0x8664u, 0u);
    EXPECT_FALSE(fmt.looksLikeRelocatableObject(view(elf)));
    EXPECT_FALSE(fmt.looksLikeRelocatableObject(view(macho)));
    EXPECT_FALSE(fmt.looksLikeRelocatableObject(view(coff)));
    EXPECT_FALSE(fmt.looksLikeRelocatableObject({}));
}
