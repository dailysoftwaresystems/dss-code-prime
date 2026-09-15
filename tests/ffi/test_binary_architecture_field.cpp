// D-FFI-RESOLVE-LIBRARY-DOES-NOT-CHECK-THE-LIBRARY-ARCH — the LOCATION TABLE,
// pinned on its own, one tier below the end-to-end refusal.
//
// `ffi::architectureFieldOf` answers "where in THIS file does its own format
// keep the CPU it was built for", and the end-to-end pins in
// `tests/program/test_ffi_resolve_library.cpp` can only ever exercise the two
// (format, architecture) pairs this repository ships object-format documents
// for. Three properties of the table are invisible from there and are exactly
// the ones that rot:
//
//   1. THE PE OFFSET IS COMPUTED, NOT CONSTANT. `e_lfanew` is a u32 file
//      offset in the DOS stub, so the COFF header sits wherever the producer
//      put it. Every real PE DSS builds happens to use one value, so an
//      end-to-end pin would stay green against a HARDCODED offset. The two
//      arms below use DIFFERENT `e_lfanew` values, so a constant reds.
//   2. THE THREE `nullopt` ARMS ARE DELIBERATE, each for its own reason (an
//      `ar` container declares no architecture, a Mach-O universal file
//      declares one PER SLICE, a 32-bit Mach-O's `cputype` lacks
//      CPU_ARCH_ABI64 and the accurate refusal for it is D-FF1-MACHO-32's).
//      An "improvement" that answered for any of them would turn a precise
//      message into a wrong one, and no end-to-end arm would notice.
//   3. A SHORT LEAD MUST BE `nullopt` RATHER THAN A GUESS — the case a
//      truncated download produces, where reading past the buffer is exactly
//      the silent-wrong-answer this boundary exists to remove.
//   4. ELF's FIELD IS SPELLED IN THE FILE'S OWN BYTE ORDER. `EI_DATA` decides
//      it, and no shipped format document is big-endian, so an end-to-end arm
//      cannot build the input that catches a little-endian-on-faith read — it
//      would print a byte-swapped number nobody could find in the file.
//
// HOST-INDEPENDENT: pure byte decoding, nothing is built and nothing is run.

#include "ffi/binary_readers/reader_common.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::span<std::uint8_t const> lead(std::vector<std::uint8_t> const& v) {
    return std::span<std::uint8_t const>{v.data(), v.size()};
}

// A 64-byte buffer opening with `magic`, zero elsewhere — the shape
// `peekLibraryIdentity` hands the table.
[[nodiscard]] std::vector<std::uint8_t> header(std::vector<std::uint8_t> magic) {
    std::vector<std::uint8_t> b(ffi::kArchitectureLeadBytes, 0u);
    for (std::size_t i = 0; i < magic.size() && i < b.size(); ++i) b[i] = magic[i];
    return b;
}

void putU32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    b[off + 0] = static_cast<std::uint8_t>(v & 0xFFu);
    b[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    b[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    b[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

constexpr std::size_t kPeLfanewOffset = 0x3Cu;

// A 64-byte ELF64 header whose `EI_DATA` and `e_machine` bytes are chosen by
// the caller. Nothing else in the file matters to the table.
[[nodiscard]] std::vector<std::uint8_t>
elfHeader(std::uint8_t eiData, std::uint8_t machineLo, std::uint8_t machineHi) {
    std::vector<std::uint8_t> b(ffi::kArchitectureLeadBytes, 0u);
    b[0] = 0x7Fu; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 2u;                      // EI_CLASS = ELFCLASS64
    b[ffi::kElfEiDataOffset] = eiData;
    b[18] = machineLo;
    b[19] = machineHi;
    return b;
}

}  // namespace

// ── ELF: a FIXED offset, and it is the gABI's ─────────────────────────────
TEST(FfiArchitectureField, ElfLocatesEMachineAtTheGabiOffset) {
    auto const b = header({0x7Fu, 'E', 'L', 'F', 2u, ffi::kElfDataLsb});
    auto const f = ffi::architectureFieldOf(ffi::FormatGuess::Elf, lead(b));
    ASSERT_TRUE(f.has_value())
        << "an ELF header must locate its own e_machine";
    EXPECT_EQ(f->offset, 18u) << "Elf64_Ehdr.e_machine is at 18 (gABI)";
    EXPECT_EQ(f->width, 2u)   << "e_machine is a u16";
    EXPECT_EQ(f->fieldName, "e_machine")
        << "the refusal names the field the operator must go and look at";
    EXPECT_FALSE(f->bigEndian)
        << "EI_DATA said ELFDATA2LSB, so the field is little-endian";
}

// ── ELF IS THE ONE FORMAT THAT DECLARES ITS OWN BYTE ORDER ────────────────
//
// The two directions of the SAME two bytes. `00 16` is EM_S390 (22) in a
// big-endian file and the meaningless 5632 read little-endian; `3E 00` is
// EM_X86_64 (62) little-endian and the meaningless 15872 big-endian. A table
// that read one lens on faith would print the other number — ✔MEASURED before
// this arm carried the order: a synthetic ELFDATA2MSB `.so` made the refusal
// claim "e_machine 0x1600 (5632)", a value that appears NOWHERE in the file.
TEST(FfiArchitectureField, ElfEMachineIsDecodedThroughTheFilesOwnEiData) {
    struct Case {
        std::uint8_t  eiData;
        std::uint8_t  lo;
        std::uint8_t  hi;
        bool          bigEndian;
        std::uint32_t decoded;
        char const*   what;
    };
    // NOLINTNEXTLINE(readability-magic-numbers) — these ARE the gABI numbers.
    constexpr Case cases[] = {
        {ffi::kElfDataMsb, 0x00u, 0x16u, true,  22u,     "EM_S390, big-endian"},
        {ffi::kElfDataMsb, 0x00u, 0xB7u, true,  183u,    "EM_AARCH64, big-endian"},
        {ffi::kElfDataLsb, 0x3Eu, 0x00u, false, 62u,     "EM_X86_64, little-endian"},
        {ffi::kElfDataMsb, 0x3Eu, 0x00u, true,  15872u,  "the LE spelling read BE"},
        {ffi::kElfDataLsb, 0x00u, 0x16u, false, 5632u,   "the BE spelling read LE"},
    };
    for (auto const& c : cases) {
        auto const b = elfHeader(c.eiData, c.lo, c.hi);
        auto const f = ffi::architectureFieldOf(ffi::FormatGuess::Elf, lead(b));
        ASSERT_TRUE(f.has_value()) << c.what;
        EXPECT_EQ(f->bigEndian, c.bigEndian) << c.what;
        auto const value = lead(b).subspan(static_cast<std::size_t>(f->offset),
                                           f->width);
        EXPECT_EQ(ffi::decodeArchitectureCode(value, *f), c.decoded)
            << c.what
            << " — the number the refusal prints must be a number that is IN "
               "the file";
    }
}

// `EI_DATA` has exactly two defined values. Anything else is a header this
// table cannot decode, and picking one of the two would be the fabrication the
// arm above exists to prevent — so it declines, exactly as `MachO32` does.
TEST(FfiArchitectureField, ElfWithAnUndefinedEiDataLocatesNothing) {
    for (std::uint8_t eiData : {std::uint8_t{0u}, std::uint8_t{3u},
                                std::uint8_t{7u}, std::uint8_t{0xFFu}}) {
        auto const b = elfHeader(eiData, 0x3Eu, 0x00u);
        EXPECT_FALSE(ffi::architectureFieldOf(ffi::FormatGuess::Elf, lead(b))
                         .has_value())
            << "EI_DATA = " << static_cast<unsigned>(eiData)
            << " is neither ELFDATA2LSB nor ELFDATA2MSB";
    }
}

// The other two formats do NOT carry an in-file order, and saying so here is
// what stops someone "generalising" the ELF arm into them: PE is
// little-endian by definition, and a Mach-O slice announces its order in the
// MAGIC, so a big-endian one never reaches this table at all.
TEST(FfiArchitectureField, PeAndMachODeclareNoInFileByteOrder) {
    auto machO = header({0xCFu, 0xFAu, 0xEDu, 0xFEu});
    auto const mf = ffi::architectureFieldOf(ffi::FormatGuess::MachO64,
                                             lead(machO));
    ASSERT_TRUE(mf.has_value());
    EXPECT_FALSE(mf->bigEndian);

    auto pe = header({'M', 'Z'});
    putU32(pe, kPeLfanewOffset, 0x80u);
    auto const pf = ffi::architectureFieldOf(ffi::FormatGuess::Pe, lead(pe));
    ASSERT_TRUE(pf.has_value());
    EXPECT_FALSE(pf->bigEndian);

    // A big-endian thin Mach-O header: `guessFormat` does not recognise it, so
    // the table is never asked and the reader's own refusal is what the
    // operator sees.
    auto const beMachO = header({0xFEu, 0xEDu, 0xFAu, 0xCFu});
    EXPECT_EQ(ffi::guessFormat(lead(beMachO)), ffi::FormatGuess::Unknown);
}

// ── Mach-O 64: also fixed, and NOT the ELF one ────────────────────────────
TEST(FfiArchitectureField, MachO64LocatesCputypeImmediatelyAfterTheMagic) {
    auto const b = header({0xCFu, 0xFAu, 0xEDu, 0xFEu});   // 0xFEEDFACF, LE
    ASSERT_EQ(ffi::guessFormat(lead(b)), ffi::FormatGuess::MachO64)
        << "precondition: these bytes really are a thin 64-bit Mach-O";
    auto const f = ffi::architectureFieldOf(ffi::FormatGuess::MachO64, lead(b));
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->offset, 4u) << "mach_header_64.cputype follows the magic";
    EXPECT_EQ(f->width, 4u)  << "cputype is a u32 (CPU_TYPE_ARM64 = 0x0100000C)";
    EXPECT_EQ(f->fieldName, "cputype");
}

// ── PE: THE OFFSET IS READ OUT OF THE FILE, which is the whole reason this
//    function returns a LOCATION instead of a value ──────────────────────────
//
// Two DIFFERENT `e_lfanew` values in one test: a hardcoded offset can satisfy
// at most one of them, so the pin reds on the regression an end-to-end arm
// (where every DSS-built PE shares one layout) would sail past.
TEST(FfiArchitectureField, PeOffsetFollowsELfanewRatherThanAConstant) {
    for (std::uint32_t lfanew : {0x80u, 0x108u}) {
        auto b = header({'M', 'Z'});
        putU32(b, kPeLfanewOffset, lfanew);
        ASSERT_EQ(ffi::guessFormat(lead(b)), ffi::FormatGuess::Pe);
        auto const f = ffi::architectureFieldOf(ffi::FormatGuess::Pe, lead(b));
        ASSERT_TRUE(f.has_value()) << "e_lfanew = " << lfanew;
        EXPECT_EQ(f->offset, lfanew + 4u)
            << "COFF FileHeader.Machine is 4 bytes past the PE signature at "
               "e_lfanew (PE/COFF 3.3); e_lfanew = "
            << lfanew;
        EXPECT_EQ(f->width, 2u) << "Machine is a u16";
        EXPECT_EQ(f->fieldName, "Machine");
    }
}

// An `e_lfanew` at the top of the u32 range must not wrap the computed offset
// into a small, plausible, WRONG location.
//
// ⚠ WHAT THIS PINS IS THE WIDTH OF THE SUM, and it is worth saying which of
// two nearby things it is NOT. `architectureFieldOf` used to carry a
// `lfanew > UINT64_MAX - 4` test with a comment claiming an "overflow-safe
// posture"; that branch could never fire — `readU32` cannot answer more than
// 0xFFFFFFFF — so it was a dead guard with a live claim, and it is gone. The
// LIVE property is that the addition happens in 64 bits: a 32-bit sum of
// 0xFFFFFFFF + 4 lands on 3, which is a perfectly seekable offset inside the
// DOS stub. The out-of-range offset is rejected where it is actually used, by
// `peekLibraryIdentity`'s `streamoff` test before the seek.
TEST(FfiArchitectureField, PeExtremeELfanewDoesNotWrapTheComputedOffset) {
    auto b = header({'M', 'Z'});
    putU32(b, kPeLfanewOffset, 0xFFFFFFFFu);
    auto const f = ffi::architectureFieldOf(ffi::FormatGuess::Pe, lead(b));
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->offset, 0xFFFFFFFFull + 4ull)
        << "the sum is computed in 64 bits; a 32-bit sum would land on 3";
}

// ── THE THREE DELIBERATE `nullopt` ARMS ───────────────────────────────────
TEST(FfiArchitectureField, ContainerAndVariantFormatsDeclareNoSingleArchitecture) {
    // An `ar` archive: the CONTAINER declares none, its MEMBERS do.
    // ⚠ AND THAT IS NOT THE SAME AS "AN ARCHIVE IS NOT CHECKED". This table
    // answers about ONE header; `ffi::checkLibraryMatchesTargetFormat` opens
    // the container and puts every member through this same table, which is
    // what the end-to-end arms in tests/program/test_ffi_resolve_library.cpp
    // pin. What is refused here is only the container-level GUESS.
    auto const ar = header({'!', '<', 'a', 'r', 'c', 'h', '>', 0x0Au});
    ASSERT_EQ(ffi::guessFormat(lead(ar)), ffi::FormatGuess::Ar);
    EXPECT_FALSE(ffi::architectureFieldOf(ffi::FormatGuess::Ar, lead(ar))
                     .has_value())
        << "an archive holds objects of possibly several architectures; "
           "answering from the GLOBAL MAGIC would be a container-level guess";

    // A Mach-O universal binary: one cputype PER SLICE, none for the file.
    auto const fat = header({0xCAu, 0xFEu, 0xBAu, 0xBEu});   // BIG-endian on disk
    ASSERT_EQ(ffi::guessFormat(lead(fat)), ffi::FormatGuess::MachOFat);
    EXPECT_FALSE(ffi::architectureFieldOf(ffi::FormatGuess::MachOFat, lead(fat))
                     .has_value())
        << "a fat_header has no cputype of its own — FF1's `lipo -thin` "
           "remediation (D-FF1-MACHO-FAT) is the message that must reach the "
           "operator here";

    // A 32-bit Mach-O: the field IS at offset 4, and answering anyway would
    // report a CPU mismatch for a file whose real defect is that FF1 does not
    // read 32-bit Mach-O.
    auto const m32 = header({0xCEu, 0xFAu, 0xEDu, 0xFEu});   // 0xFEEDFACE, LE
    ASSERT_EQ(ffi::guessFormat(lead(m32)), ffi::FormatGuess::MachO32);
    EXPECT_FALSE(ffi::architectureFieldOf(ffi::FormatGuess::MachO32, lead(m32))
                     .has_value())
        << "its cputype lacks CPU_ARCH_ABI64, so the comparison would be "
           "against a value from a different width class — D-FF1-MACHO-32 owns "
           "this refusal";

    EXPECT_FALSE(ffi::architectureFieldOf(ffi::FormatGuess::Unknown,
                                          lead(header({0u})))
                     .has_value());
}

// ── A SHORT LEAD IS `nullopt`, NEVER A READ PAST THE BUFFER ───────────────
TEST(FfiArchitectureField, TruncatedHeadersLocateNothing) {
    std::vector<std::uint8_t> const elf19{
        0x7Fu, 'E', 'L', 'F', 2u, ffi::kElfDataLsb, 0, 0, 0, 0,
        0,     0,   0,   0,   0,  0,                0, 0, 0};
    ASSERT_EQ(elf19.size(), 19u) << "one byte short of the e_machine field's end";
    EXPECT_FALSE(ffi::architectureFieldOf(ffi::FormatGuess::Elf, lead(elf19))
                     .has_value());

    std::vector<std::uint8_t> const macho7{0xCFu, 0xFAu, 0xEDu, 0xFEu, 0, 0, 0};
    EXPECT_FALSE(ffi::architectureFieldOf(ffi::FormatGuess::MachO64, lead(macho7))
                     .has_value());

    // A PE truncated before `e_lfanew` itself: the offset is unknowable, so
    // there is nothing to locate and nothing to guess.
    std::vector<std::uint8_t> pe63(63u, 0u);
    pe63[0] = 'M';
    pe63[1] = 'Z';
    EXPECT_FALSE(ffi::architectureFieldOf(ffi::FormatGuess::Pe, lead(pe63))
                     .has_value())
        << "e_lfanew ends at 0x40, so 63 bytes cannot supply it";
}

// The window constant is a CLAIM about the three formats above, so assert it
// against them rather than leaving it to be re-derived by the next reader.
TEST(FfiArchitectureField, TheLeadWindowCoversEveryFormatThatDeclaresOne) {
    EXPECT_GE(ffi::kArchitectureLeadBytes, 20u) << "ELF e_machine ends at 20";
    EXPECT_GE(ffi::kArchitectureLeadBytes, 8u)  << "Mach-O cputype ends at 8";
    EXPECT_GE(ffi::kArchitectureLeadBytes, kPeLfanewOffset + 4u)
        << "PE e_lfanew ends at 0x40";
}
