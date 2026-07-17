// GNU / System V `ar` static-archive WRITER tests -- c163, the
// D-LK-STATIC-ARCHIVE-WRITER anchor (writer half of D-FF1-AR-WRITER-STATIC-
// LINK). `dss::link::format::writeArArchive` is the EXACT INVERSE of the c161
// reader (`dss::ffi::readArArchive`), so the two paired end-to-end is the
// strongest self-contained oracle -- write bytes, read them back, assert the
// armap + member list survive exactly.
//
// Pins:
//   * Byte-exact framing: magic; the "/" armap ar_hdr (name, ASCII-decimal
//     size, "`\n" trailer); the armap payload (BIG-endian u32 count + u32
//     member-header offsets + NUL-terminated name blob); a regular member's
//     60-byte header (name "lib.o/", octal mode "644", ASCII-decimal size);
//     the "//" long-name table + a "/N" name field for a >15-char member.
//   * Round-trip through the SHIPPED c161 reader (W1): armap lists
//     dss_lib_answer + dss_data -> member lib.o; member list + sizes exact.
//   * Red-on-disable: a WRONG-ENDIAN armap count (BE -> LE) breaks the
//     round-trip (the reader rejects the bogus offset array); a dropped index
//     (count 0) surfaces no symbols -- so the RoundTrip symbol asserts are the
//     "armap emitted" pin (their inverse is "no armap -> no symbols").
//   * Multi-member + long name (W3): a 2-member archive with a >15-char member
//     name forces the "//" table; both members + both armap symbols resolve.
//   * Fail-loud belts: empty / framing-reserved member name, empty exported
//     symbol name.
//   * End-to-end through the shipped pipeline composition
//     `linkAndWriteStaticArchive`: a REAL DSS-emitted ELF ET_REL `.o` member
//     (via `elf::encode`) bundled into a `.a` on disk, read back by the c161
//     reader -> the armap lists the module's externally-visible symbols.
//
// The W2 real-GNU-`ar`/`nm` cross-check runs OUT-OF-BAND (WSL) against the
// artifacts the DISABLED `WriteRealArchivesForWslWitness` case drops to disk --
// the suite stays hermetic (no shelling to a toolchain, mirroring the c161
// reader tests' CI-hermeticity note).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/binary_readers/ar_reader.hpp"
#include "link/format/ar.hpp"
#include "link/format/elf.hpp"
#include "link/object_format_schema.hpp"
#include "program/compile_pipeline.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace dss;
using dss::link::format::ArMemberInput;
using dss::link::format::writeArArchive;

namespace {

// -- Byte-reading helpers (the armap is the ONE big-endian structure) --------

[[nodiscard]] std::uint32_t readU32BE(std::span<std::uint8_t const> b,
                                      std::size_t off) {
    return (static_cast<std::uint32_t>(b[off + 0]) << 24)
         | (static_cast<std::uint32_t>(b[off + 1]) << 16)
         | (static_cast<std::uint32_t>(b[off + 2]) <<  8)
         |  static_cast<std::uint32_t>(b[off + 3]);
}

// A raw ar_hdr field as text (trailing spaces stripped -- the reader's rstrip).
[[nodiscard]] std::string field(std::span<std::uint8_t const> b,
                                std::size_t off, std::size_t len) {
    std::string s{reinterpret_cast<char const*>(&b[off]), len};
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

// One member exporting the two W1 symbols, with an opaque 20-byte payload
// (the writer is format-blind; the reader never parses member contents).
[[nodiscard]] ArMemberInput w1Member() {
    return ArMemberInput{"lib.o",
                         std::vector<std::uint8_t>(20, 0xAAu),
                         {"dss_lib_answer", "dss_data"}};
}

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

// The RELOCATABLE ELF format -- an ET_REL `.o` is exactly what an `ar` member
// is (an image flavor would fail `linkAndWriteStaticArchive`'s relocatable gate).
[[nodiscard]] Loaded loadRelocatableElf() {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) { ADD_FAILURE() << "loadShipped(x86_64) failed"; return out; }
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    if (!f.has_value()) { ADD_FAILURE() << "loadShipped(elf64-x86_64-linux) failed"; return out; }
    out.target = std::move(t).value();
    out.format = std::move(f).value();
    return out;
}

// A minimal AssembledModule of N `ret`-only functions, each a Global (externally
// visible) symbol with the given name -- the armap-export surface of a real .o.
[[nodiscard]] AssembledModule makeFnModule(std::vector<std::string> const& names) {
    AssembledModule mod;
    mod.expectedFuncCount = names.size();
    for (std::size_t i = 0; i < names.size(); ++i) {
        AssembledFunction fn;
        fn.symbol = SymbolId{static_cast<std::uint32_t>(10 + i)};
        fn.bytes  = {0xC3};   // ret
        mod.functions.push_back(std::move(fn));
        mod.symbols.push_back(ModuleSymbol{SymbolId{static_cast<std::uint32_t>(10 + i)},
                                           names[i], SymbolBinding::Global,
                                           SymbolVisibility::Default});
    }
    return mod;
}

} // namespace

// -- Byte-exact framing pins --------------------------------------------------

TEST(ArWriter, MagicAndArmapBytePins) {
    ArMemberInput m = w1Member();
    DiagnosticReporter rep;
    auto ar = writeArArchive(std::span<ArMemberInput const>{&m, 1}, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(ar.size(), 8u + 60u);

    // Global magic.
    std::string const magic{reinterpret_cast<char const*>(ar.data()), 8};
    EXPECT_EQ(magic, "!<arch>\n");

    // "/" armap header at offset 8. Fields: name(16) mtime(12) uid(6) gid(6)
    // mode(8) size(10) + "`\n".
    EXPECT_EQ(field(ar, 8, 16), "/");
    // armap payload = count(4) + offset(4)*2 + "dss_lib_answer\0"(15) +
    // "dss_data\0"(9) = 4 + 8 + 24 = 36.
    EXPECT_EQ(field(ar, 8 + 48, 10), "36");
    EXPECT_EQ(ar[8 + 58], 0x60u);
    EXPECT_EQ(ar[8 + 59], 0x0Au);

    // Armap payload at 68: BE count then BE member-header offsets.
    EXPECT_EQ(readU32BE(ar, 68), 2u);                 // count
    // Member header sits at 8 + 60 + 36 (armap even, no pad) = 104.
    constexpr std::uint32_t kMemberHdrOff = 104u;
    EXPECT_EQ(readU32BE(ar, 72), kMemberHdrOff);      // offset[0] -> lib.o
    EXPECT_EQ(readU32BE(ar, 76), kMemberHdrOff);      // offset[1] -> lib.o
    // NUL-terminated name blob at 80.
    std::string const blob{reinterpret_cast<char const*>(&ar[80]), 24};
    EXPECT_EQ(blob, std::string("dss_lib_answer\0dss_data\0", 24));

    // The member header at 104: name "lib.o/", octal mode "644", size "20".
    EXPECT_EQ(field(ar, kMemberHdrOff, 16), "lib.o/");
    EXPECT_EQ(field(ar, kMemberHdrOff + 40, 8), "644");
    EXPECT_EQ(field(ar, kMemberHdrOff + 48, 10), "20");
    EXPECT_EQ(ar[kMemberHdrOff + 58], 0x60u);
    EXPECT_EQ(ar[kMemberHdrOff + 59], 0x0Au);
    // Member payload (20 bytes, even) -> archive ends at 104+60+20 = 184.
    EXPECT_EQ(ar.size(), 184u);
}

TEST(ArWriter, TwoByteAlignmentPadsOddPayload) {
    // An odd-length payload gets a '\n' pad byte so the next header is even.
    ArMemberInput m{"a.o", std::vector<std::uint8_t>(5, 0x11u), {"s"}};
    DiagnosticReporter rep;
    auto ar = writeArArchive(std::span<ArMemberInput const>{&m, 1}, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // armap payload = 4 + 4 + "s\0"(2) = 10 (even). Member header at 8+60+10=78.
    constexpr std::size_t kMemberHdrOff = 78u;
    EXPECT_EQ(field(ar, kMemberHdrOff + 48, 10), "5");        // size = exact 5
    // 5 payload bytes + 1 '\n' pad -> total 8+60+10 + 60+5+1 = 144.
    ASSERT_EQ(ar.size(), 144u);
    EXPECT_EQ(ar.back(), 0x0Au);                              // the pad byte
}

// -- Round-trip through the shipped c161 reader (W1) --------------------------

TEST(ArWriter, RoundTripThroughC161Reader) {
    ArMemberInput m = w1Member();
    DiagnosticReporter wrep;
    auto ar = writeArArchive(std::span<ArMemberInput const>{&m, 1}, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);

    DiagnosticReporter rrep;
    auto arch = ffi::readArArchive(ar, "libdsslib.a", rrep);
    ASSERT_TRUE(arch.has_value()) << arch.error().detail;

    // Member list: [lib.o], size 20 ("/" armap excluded from the member list).
    ASSERT_EQ(arch->members.size(), 1u);
    EXPECT_EQ(arch->members[0].name, "lib.o");
    EXPECT_EQ(arch->members[0].size, 20u);

    // Armap: BOTH symbols map to member 0 (the "armap emitted -> reader sees
    // symbols" pin; its inverse -- a dropped index -- surfaces zero symbols).
    ASSERT_EQ(arch->symbols.size(), 2u);
    EXPECT_EQ(arch->symbols[0].name, "dss_lib_answer");
    EXPECT_EQ(arch->symbols[0].memberIndex, 0u);
    EXPECT_EQ(arch->symbols[1].name, "dss_data");
    EXPECT_EQ(arch->symbols[1].memberIndex, 0u);
    EXPECT_EQ(arch->symbols[0].memberOffset, arch->members[0].headerOffset);
}

TEST(ArWriter, WrongEndianArmapCountBreaksRoundTrip_RedOnDisable) {
    // The BE armap count is load-bearing: emit it little-endian and the c161
    // reader reads a bogus huge count whose offset array overruns the armap.
    ArMemberInput m = w1Member();
    DiagnosticReporter wrep;
    auto ar = writeArArchive(std::span<ArMemberInput const>{&m, 1}, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_EQ(readU32BE(ar, 68), 2u);

    // Byte-swap the count field 00 00 00 02 -> 02 00 00 00 (as a LE writer would).
    std::swap(ar[68], ar[71]);
    std::swap(ar[69], ar[70]);

    DiagnosticReporter rrep;
    auto arch = ffi::readArArchive(ar, "wrongendian.a", rrep);
    ASSERT_FALSE(arch.has_value());
    EXPECT_EQ(arch.error().kind, ffi::BinaryReadErrorKind::CorruptedBinary);
}

// -- Multi-member + GNU long name (W3) ---------------------------------------

TEST(ArWriter, LongMemberNameForcesLongNameTable) {
    // A name > 15 bytes cannot fit "name/" in the 16-byte field -> the "//"
    // GNU long-name table + a "/N" back-reference.
    std::string const longName = "this_is_a_very_long_member_name.o";   // 33 chars
    ArMemberInput a{"a.o", std::vector<std::uint8_t>(4, 0x01u), {"a_sym"}};
    ArMemberInput b{longName, std::vector<std::uint8_t>(4, 0x02u), {"long_named_symbol"}};
    std::vector<ArMemberInput> members{a, b};
    DiagnosticReporter wrep;
    auto ar = writeArArchive(members, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);

    // A "//" special member must be present.
    DiagnosticReporter rrep;
    auto arch = ffi::readArArchive(ar, "liblong.a", rrep);
    ASSERT_TRUE(arch.has_value()) << arch.error().detail;
    ASSERT_EQ(arch->members.size(), 2u);
    EXPECT_EQ(arch->members[0].name, "a.o");
    EXPECT_EQ(arch->members[1].name, longName);   // resolved via the "//" table

    ASSERT_EQ(arch->symbols.size(), 2u);
    EXPECT_EQ(arch->symbols[0].name, "a_sym");
    EXPECT_EQ(arch->symbols[0].memberIndex, 0u);
    EXPECT_EQ(arch->symbols[1].name, "long_named_symbol");
    EXPECT_EQ(arch->symbols[1].memberIndex, 1u);   // -> the long-named member

    // The short member's name field is inline "a.o/"; the long one's is "/N".
    EXPECT_EQ(field(ar, arch->members[0].headerOffset, 16), "a.o/");
    std::string const longField = field(ar, arch->members[1].headerOffset, 16);
    ASSERT_FALSE(longField.empty());
    EXPECT_EQ(longField[0], '/');
    EXPECT_NE(longField, "//");   // a real "/N" ref, not the table special
}

TEST(ArWriter, EmptyMembersYieldsMagicPlusEmptyArmap) {
    DiagnosticReporter wrep;
    auto ar = writeArArchive(std::span<ArMemberInput const>{}, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    // magic(8) + "/" header(60) + count(4, == 0) = 72.
    ASSERT_EQ(ar.size(), 72u);
    EXPECT_EQ(field(ar, 8, 16), "/");
    EXPECT_EQ(readU32BE(ar, 68), 0u);
    // The reader accepts it: no members, no symbols.
    DiagnosticReporter rrep;
    auto arch = ffi::readArArchive(ar, "empty.a", rrep);
    ASSERT_TRUE(arch.has_value()) << arch.error().detail;
    EXPECT_TRUE(arch->members.empty());
    EXPECT_TRUE(arch->symbols.empty());
}

// -- Fail-loud belts ----------------------------------------------------------

TEST(ArWriter, RejectsEmptyMemberName) {
    ArMemberInput m{"", {0x00u}, {}};
    DiagnosticReporter rep;
    auto ar = writeArArchive(std::span<ArMemberInput const>{&m, 1}, rep);
    EXPECT_TRUE(ar.empty());
    EXPECT_GT(rep.errorCount(), 0u);
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::K_ArchiveMemberNameInvalid) saw = true;
    EXPECT_TRUE(saw);
}

TEST(ArWriter, RejectsSlashInMemberName) {
    ArMemberInput m{"dir/lib.o", {0x00u}, {}};   // '/' would corrupt framing
    DiagnosticReporter rep;
    auto ar = writeArArchive(std::span<ArMemberInput const>{&m, 1}, rep);
    EXPECT_TRUE(ar.empty());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::K_ArchiveMemberNameInvalid) saw = true;
    EXPECT_TRUE(saw);
}

TEST(ArWriter, RejectsEmptyExportedSymbolName) {
    ArMemberInput m{"lib.o", {0x00u, 0x01u}, {"good", ""}};
    DiagnosticReporter rep;
    auto ar = writeArArchive(std::span<ArMemberInput const>{&m, 1}, rep);
    EXPECT_TRUE(ar.empty());
    bool saw = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::K_ArchiveMemberNameInvalid) saw = true;
    EXPECT_TRUE(saw);
}

// -- End-to-end through the shipped pipeline composition ----------------------

TEST(ArWriter, PipelineStaticArchiveFromRealDssObject) {
    Loaded loaded = loadRelocatableElf();
    ASSERT_TRUE(loaded.target && loaded.format);

    // A real DSS ELF ET_REL member exporting dss_lib_answer + dss_data.
    AssembledModule mod = makeFnModule({"dss_lib_answer", "dss_data"});
    std::string const memberName = "lib.o";

    auto const outPath = std::filesystem::temp_directory_path()
                       / "dss_ar_pipeline_test_libdsslib.a";
    std::error_code ec;
    std::filesystem::remove(outPath, ec);

    DiagnosticReporter rep;
    bool const ok = linkAndWriteStaticArchive(
        std::span<AssembledModule const>{&mod, 1},
        std::span<std::string const>{&memberName, 1},
        *loaded.target, *loaded.format, outPath, rep);
    ASSERT_TRUE(ok) << "linkAndWriteStaticArchive failed; errs="
                    << rep.errorCount();

    // Read the on-disk `.a` back with the c161 reader.
    std::ifstream in(outPath, std::ios::binary);
    ASSERT_TRUE(in);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(outPath, ec);

    DiagnosticReporter rrep;
    auto arch = ffi::readArArchive(bytes, "libdsslib.a", rrep);
    ASSERT_TRUE(arch.has_value()) << arch.error().detail;
    ASSERT_EQ(arch->members.size(), 1u);
    EXPECT_EQ(arch->members[0].name, "lib.o");
    ASSERT_EQ(arch->symbols.size(), 2u);
    EXPECT_EQ(arch->symbols[0].name, "dss_lib_answer");
    EXPECT_EQ(arch->symbols[1].name, "dss_data");
    EXPECT_EQ(arch->symbols[0].memberIndex, 0u);
    EXPECT_EQ(arch->symbols[1].memberIndex, 0u);
}

TEST(ArWriter, PipelineRejectsImageFlavorFormat) {
    // A static archive bundles RELOCATABLE objects; an image-flavor format
    // (.so) is rejected loud.
    auto t = TargetSchema::loadShipped("x86_64");
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(t.has_value() && f.has_value());
    AssembledModule mod = makeFnModule({"dss_lib_answer"});
    std::string const name = "lib.o";
    auto const outPath = std::filesystem::temp_directory_path()
                       / "dss_ar_reject_imageflavor.a";
    DiagnosticReporter rep;
    bool const ok = linkAndWriteStaticArchive(
        std::span<AssembledModule const>{&mod, 1},
        std::span<std::string const>{&name, 1},
        *t.value(), *f.value(), outPath, rep);
    EXPECT_FALSE(ok);
    EXPECT_GT(rep.errorCount(), 0u);
}

// -- W2/W3 real-GNU-ar/nm witness artifact drop (DISABLED; run out-of-band) ---
//
// Produces DSS-written `.a` files for the WSL `ar t` / `nm -s` cross-check.
// DISABLED so CI stays hermetic; run explicitly:
//   test_ar_writer --gtest_also_run_disabled_tests \
//                  --gtest_filter='*WriteRealArchivesForWslWitness*'
// Output dir: $DSS_AR_WITNESS_DIR (else the system temp dir); paths printed.
TEST(ArWriter, DISABLED_WriteRealArchivesForWslWitness) {
    Loaded loaded = loadRelocatableElf();
    ASSERT_TRUE(loaded.target && loaded.format);

    char const* dir = std::getenv("DSS_AR_WITNESS_DIR");
    std::filesystem::path const outDir =
        dir ? std::filesystem::path{dir} : std::filesystem::temp_directory_path();
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    // W2: single member lib.o exporting dss_lib_answer + dss_data. Give
    // dss_lib_answer a real `mov eax,42; ret` body so a foreign `gcc main.c
    // -l:libdsslib.a` that pulls it from the armap RUNS -> exit 42 (an
    // unambiguous end-to-end witness that real ld resolved + pulled the member).
    {
        AssembledModule mod = makeFnModule({"dss_lib_answer", "dss_data"});
        mod.functions[0].bytes = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};  // mov eax,42; ret
        std::string const name = "lib.o";
        auto const p = outDir / "libdsslib.a";
        DiagnosticReporter rep;
        ASSERT_TRUE(linkAndWriteStaticArchive(
            std::span<AssembledModule const>{&mod, 1},
            std::span<std::string const>{&name, 1},
            *loaded.target, *loaded.format, p, rep)) << "errs=" << rep.errorCount();
        std::cout << "[witness] wrote " << p.string() << "\n";
    }

    // W3: two members, the second with a >15-char name -> the "//" table.
    {
        AssembledModule a = makeFnModule({"a_answer"});
        AssembledModule b = makeFnModule({"long_named_symbol"});
        std::vector<AssembledModule> mods;
        mods.push_back(std::move(a));
        mods.push_back(std::move(b));
        std::vector<std::string> names{"a.o", "this_is_a_very_long_member_name.o"};
        auto const p = outDir / "liblong.a";
        DiagnosticReporter rep;
        ASSERT_TRUE(linkAndWriteStaticArchive(
            mods, names, *loaded.target, *loaded.format, p, rep))
            << "errs=" << rep.errorCount();
        std::cout << "[witness] wrote " << p.string() << "\n";
    }
}
